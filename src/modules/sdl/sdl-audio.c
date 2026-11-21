#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

#define SDL_MAX_GET_SAMPLES (1 << 24)

typedef struct {
    SDL_AudioStream* s;
    int bytes_per_sample;
    int is_float;
    int channels;
    int recording;
} sdl_audio;

static void sdl_audio_dispose(void* p)
{
    sdl_audio* a = p;
    if (a->s) {
        SDL_DestroyAudioStream(a->s);
        atomic_fetch_sub(&sdl_live_streams, 1);
    }
    dyn_nat_free(a);
}

static const char* const sdl_audio_keys[] = { "freq", "channels", "format", "recording", "device" };

JSValue sdl_audio_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    SDL_AudioSpec spec;
    int freq = 48000, ch = 2, is_float = 1, recording = 0;
    SDL_AudioDeviceID dev = 0;
    sdl_audio* a;
    JSValue v;
    if (!JS_IsObject(opts) && !JS_IsUndefined(opts))
        return JS_ThrowTypeError(ctx, "dyna:sdl: new AudioStream({ freq?, channels?, format?, recording?, device? })");
    if (sdl_check_opts(ctx, opts, "AudioStream", sdl_audio_keys, (int)countof(sdl_audio_keys)))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        v = JS_GetPropertyStr(ctx, opts, "freq");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && sdl_int_in(ctx, v, "AudioStream freq", 8000, 384000, &freq)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "channels");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && sdl_int_in(ctx, v, "AudioStream channels", 1, 8, &ch)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "format");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char* s;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "dyna:sdl: AudioStream format is \"f32\" or \"s16\"");
            }
            s = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "f32"))
                is_float = 1;
            else if (!strcmp(s, "s16"))
                is_float = 0;
            else {
                JS_FreeCString(ctx, s);
                return JS_ThrowRangeError(ctx, "dyna:sdl: AudioStream format is \"f32\" or \"s16\"");
            }
            JS_FreeCString(ctx, s);
        }
        if (sdl_opt_bool(ctx, opts, "recording", &recording))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, opts, "device");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            double d;
            int rc = JS_ToFloat64(ctx, &d, v);
            JS_FreeValue(ctx, v);
            if (rc)
                return JS_EXCEPTION;
            if (!(d >= 1 && d <= 4294967295.0) || d != floor(d))
                return JS_ThrowRangeError(ctx, "dyna:sdl: AudioStream device must be an id from audioDevices()");
            dev = (SDL_AudioDeviceID)d;
        }
    }
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        return JS_ThrowTypeError(ctx, "dyna:sdl: new AudioStream() needs init([\"audio\"]) first");
    if (dev && !SDL_GetAudioDeviceName(dev))
        return JS_ThrowRangeError(ctx, "dyna:sdl: AudioStream: no audio device has id %u (see audioDevices())", (unsigned)dev);
    if (dev && SDL_IsAudioDevicePlayback(dev) == !!recording)
        return JS_ThrowTypeError(ctx, "dyna:sdl: AudioStream device %u is a %s device but recording is %s", (unsigned)dev,
            recording ? "playback" : "recording", recording ? "true" : "false");
    a = dyn_nat_calloc(1, sizeof(*a));
    if (!a)
        return JS_ThrowOutOfMemory(ctx);
    spec.format = is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16;
    spec.channels = ch;
    spec.freq = freq;
    if (!dev)
        dev = recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    a->s = SDL_OpenAudioDeviceStream(dev, &spec, NULL, NULL);
    if (!a->s) {
        dyn_nat_free(a);
        return sdl_throw(ctx, "SDL_OpenAudioDeviceStream");
    }
    a->is_float = is_float;
    a->bytes_per_sample = is_float ? 4 : 2;
    a->channels = ch;
    a->recording = recording;
    atomic_fetch_add(&sdl_live_streams, 1);
    return dyn_res_wrap(ctx, new_target, sdl_audio_cid, a, sdl_audio_dispose);
}

JSValue sdl_audio_put(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdl_audio* a;
    JSClassID cid;
    size_t off, blen, bpe, ab;
    JSValue buf;
    uint8_t* base;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: put(samples) needs a Float32Array (f32) or Int16Array (s16)");
    a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    if (!a)
        return JS_EXCEPTION;
    if (a->recording)
        return JS_ThrowTypeError(ctx, "dyna:sdl: put() feeds a playback stream; a recording stream is read with get()");
    cid = JS_GetClassID(argv[0]);
    if (cid != (a->is_float ? sdl_f32_cid : sdl_i16_cid))
        return JS_ThrowTypeError(ctx, "dyna:sdl: this stream's format is %s, so put() takes a %s",
            a->is_float ? "f32" : "s16", a->is_float ? "Float32Array" : "Int16Array");
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (off > ab || blen > ab - off || blen > INT32_MAX)
        return JS_ThrowRangeError(ctx, "dyna:sdl: put: the samples do not fit one call");
    if (blen && !SDL_PutAudioStreamData(a->s, base + off, (int)blen))
        return sdl_throw(ctx, "SDL_PutAudioStreamData");
    return JS_UNDEFINED;
}

JSValue sdl_audio_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_audio* a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    bool ok = false;
    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_ResumeAudioStreamDevice(a->s);
        break;
    case 1:
        ok = SDL_PauseAudioStreamDevice(a->s);
        break;
    case 2:
        ok = SDL_ClearAudioStream(a->s);
        break;
    case 3:
        ok = SDL_FlushAudioStream(a->s);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, "an audio stream operation");
    return JS_UNDEFINED;
}

JSValue sdl_audio_queued(JSContext* ctx, JSValueConst this_val)
{
    sdl_audio* a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    int n;
    if (!a)
        return JS_EXCEPTION;
    n = SDL_GetAudioStreamQueued(a->s);
    if (n < 0)
        return sdl_throw(ctx, "SDL_GetAudioStreamQueued");
    return JS_NewInt32(ctx, n);
}

JSValue sdl_audio_set_gain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double g;
    sdl_audio* a;
    if (argc < 1 || JS_ToFloat64(ctx, &g, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "dyna:sdl: setGain(gain)") : JS_EXCEPTION;
    if (!(g >= 0 && g <= 16))
        return JS_ThrowRangeError(ctx, "dyna:sdl: setGain: gain must be in 0..16");
    a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    if (!a)
        return JS_EXCEPTION;
    if (!SDL_SetAudioStreamGain(a->s, (float)g))
        return sdl_throw(ctx, "SDL_SetAudioStreamGain");
    return JS_UNDEFINED;
}

JSValue sdl_audio_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdl_audio* a;
    int max = SDL_MAX_GET_SAMPLES, avail, n, got, frame;
    uint8_t* buf;
    JSValue ta;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && sdl_int_in(ctx, argv[0], "get(maxSamples)", 1, SDL_MAX_GET_SAMPLES, &max))
        return JS_EXCEPTION;
    a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    if (!a)
        return JS_EXCEPTION;
    if (!a->recording)
        return JS_ThrowTypeError(ctx, "dyna:sdl: get() reads a recording stream; a playback stream is fed with put()");
    avail = SDL_GetAudioStreamAvailable(a->s);
    if (avail < 0)
        return sdl_throw(ctx, "SDL_GetAudioStreamAvailable");
    frame = a->channels;
    n = avail / a->bytes_per_sample;
    if (n > max)
        n = max;
    n -= n % frame;
    buf = dyn_nat_malloc(n ? (size_t)n * (size_t)a->bytes_per_sample : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    got = n ? SDL_GetAudioStreamData(a->s, buf, n * a->bytes_per_sample) : 0;
    if (got < 0) {
        dyn_nat_free(buf);
        return sdl_throw(ctx, "SDL_GetAudioStreamData");
    }
    ta = sdl_samples_array(ctx, buf, (size_t)got - (size_t)got % (size_t)a->bytes_per_sample, a->is_float);
    dyn_nat_free(buf);
    return ta;
}

JSValue sdl_audio_flag(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_audio* a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    int n;
    if (!a)
        return JS_EXCEPTION;
    if (magic == 1)
        return JS_NewBool(ctx, a->recording);
    n = SDL_GetAudioStreamAvailable(a->s);
    if (n < 0)
        return sdl_throw(ctx, "SDL_GetAudioStreamAvailable");
    return JS_NewInt32(ctx, n);
}

JSValue sdl_audio_devices(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "recording" };
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    int recording = 0, n = 0, i;
    SDL_AudioDeviceID* ids;
    JSValue arr;
    (void)this_val;
    if (sdl_check_opts(ctx, opts, "audioDevices", keys, (int)countof(keys)) || sdl_opt_bool(ctx, opts, "recording", &recording))
        return JS_EXCEPTION;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        return JS_ThrowTypeError(ctx, "dyna:sdl: audioDevices() needs init([\"audio\"]) first");
    ids = recording ? SDL_GetAudioRecordingDevices(&n) : SDL_GetAudioPlaybackDevices(&n);
    if (!ids)
        return sdl_throw(ctx, recording ? "SDL_GetAudioRecordingDevices" : "SDL_GetAudioPlaybackDevices");
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        if (sdl_set_int(ctx, o, "id", (double)ids[i]) < 0
            || JS_DefinePropertyValueStr(ctx, o, "name", JS_NewString(ctx, name ? name : ""), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    SDL_free(ids);
    return arr;
}

#endif
