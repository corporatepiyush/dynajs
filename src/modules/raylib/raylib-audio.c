#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

int rl_audio_ready(JSContext* ctx, const char* what)
{
    if (!rl_on_main(ctx, what))
        return 0;
    if (!IsAudioDeviceReady()) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs initAudio() first", what);
        return 0;
    }
    return 1;
}

static void rl_snd_link(rl_snd* s)
{
    s->prev = NULL;
    s->next = rl.snd;
    if (rl.snd)
        rl.snd->prev = s;
    rl.snd = s;
}

static void rl_snd_unlink(rl_snd* s)
{
    if (s->prev)
        s->prev->next = s->next;
    else if (rl.snd == s)
        rl.snd = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static void rl_snd_unload(rl_snd* s)
{
    if (!s->alive)
        return;
    if (s->kind == RL_SOUND)
        UnloadSound(s->sound);
    else if (s->kind == RL_MUSIC)
        UnloadMusicStream(s->music);
    else
        UnloadAudioStream(s->stream);
    s->alive = 0;
}

static void rl_snd_dispose(void* p)
{
    rl_snd* s = p;
    rl_snd_unload(s);
    rl_snd_unlink(s);
    dyn_nat_free(s);
}

rl_snd* rl_snd_of(JSContext* ctx, JSValueConst v, JSClassID cid)
{
    rl_snd* s = dyn_res_native(ctx, v, cid);
    if (s && !s->alive) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the audio device was closed under this resource");
        return NULL;
    }
    return s;
}

JSValue rl_audio_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    float v = 1;
    rl_snd* s;
    (void)this_val;
    if (magic == 3 && (rl_args(ctx, argc, argv, 1, "setMasterVolume(volume)") || rl_num_in(ctx, argv[0], "setMasterVolume volume", 0, 1, &v)))
        return JS_EXCEPTION;
    if (!rl_on_main(ctx, "the audio device"))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!IsAudioDeviceReady())
            InitAudioDevice();
        if (!IsAudioDeviceReady())
            return JS_ThrowInternalError(ctx, "dyna:raylib: initAudio() found no audio device");
        return JS_UNDEFINED;
    case 1:
        s = rl.snd;
        while (s) {
            rl_snd* next = s->next;
            rl_snd_unload(s);
            s->next = s->prev = NULL;
            s = next;
        }
        rl.snd = NULL;
        if (IsAudioDeviceReady())
            CloseAudioDevice();
        return JS_UNDEFINED;
    case 2:
        return JS_NewBool(ctx, IsAudioDeviceReady());
    default:
        if (!rl_audio_ready(ctx, "setMasterVolume()"))
            return JS_EXCEPTION;
        SetMasterVolume(v);
        return JS_UNDEFINED;
    }
}

JSValue rl_snd_wrap(JSContext* ctx, JSClassID cid, rl_snd* s)
{
    s->alive = 1;
    rl_snd_link(s);
    return dyn_res_wrap(ctx, JS_UNDEFINED, cid, s, rl_snd_dispose);
}

JSValue rl_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSClassID cid = JS_GetClassID(this_val);
    rl_snd* s;
    (void)argc;
    (void)argv;
    if (!rl_audio_ready(ctx, "an audio method"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, cid == rl_music_cid ? rl_music_cid : cid == rl_stream_cid ? rl_stream_cid
                                                                                           : rl_sound_cid);
    if (!s)
        return JS_EXCEPTION;
    switch (s->kind * 4 + magic) {
    case RL_SOUND * 4 + 0:
        PlaySound(s->sound);
        break;
    case RL_SOUND * 4 + 1:
        StopSound(s->sound);
        break;
    case RL_SOUND * 4 + 2:
        PauseSound(s->sound);
        break;
    case RL_SOUND * 4 + 3:
        ResumeSound(s->sound);
        break;
    case RL_MUSIC * 4 + 0:
        PlayMusicStream(s->music);
        break;
    case RL_MUSIC * 4 + 1:
        StopMusicStream(s->music);
        break;
    case RL_MUSIC * 4 + 2:
        PauseMusicStream(s->music);
        break;
    case RL_MUSIC * 4 + 3:
        ResumeMusicStream(s->music);
        break;
    case RL_STREAM * 4 + 0:
        PlayAudioStream(s->stream);
        break;
    case RL_STREAM * 4 + 1:
        StopAudioStream(s->stream);
        break;
    case RL_STREAM * 4 + 2:
        PauseAudioStream(s->stream);
        break;
    case RL_STREAM * 4 + 3:
        ResumeAudioStream(s->stream);
        break;
    }
    return JS_UNDEFINED;
}

JSValue rl_snd_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    JSClassID cid = JS_GetClassID(this_val);
    rl_snd* s;
    if (!rl_audio_ready(ctx, "an audio property"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, cid == rl_music_cid ? rl_music_cid : cid == rl_stream_cid ? rl_stream_cid
                                                                                           : rl_sound_cid);
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, s->kind == RL_SOUND ? IsSoundPlaying(s->sound) : s->kind == RL_MUSIC ? IsMusicStreamPlaying(s->music)
                                                                                                    : IsAudioStreamPlaying(s->stream));
    case 1:
        return JS_NewFloat64(ctx, (double)GetMusicTimeLength(s->music));
    case 2:
        return JS_NewFloat64(ctx, (double)GetMusicTimePlayed(s->music));
    default:
        return JS_NewBool(ctx, IsAudioStreamProcessed(s->stream));
    }
}

JSValue rl_snd_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "setVolume(volume)", "setPitch(pitch)", "setPan(pan)", "seek(seconds)", "setLooping(on)" };
    static const double lo[] = { 0, 0.01, 0, 0, 0 }, hi[] = { 1, 16, 1, 1e7, 1 };
    JSClassID cid = JS_GetClassID(this_val);
    float v = 0;
    int on = 0;
    rl_snd* s;
    if (rl_args(ctx, argc, argv, 1, names[magic]))
        return JS_EXCEPTION;
    if (magic == 4) {
        if (!JS_IsBool(argv[0]))
            return JS_ThrowTypeError(ctx, "dyna:raylib: setLooping(on) needs a boolean");
        on = JS_ToBool(ctx, argv[0]);
    } else if (rl_num_in(ctx, argv[0], names[magic], lo[magic], hi[magic], &v)) {
        return JS_EXCEPTION;
    }
    if (!rl_audio_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, cid == rl_music_cid ? rl_music_cid : cid == rl_stream_cid ? rl_stream_cid
                                                                                           : rl_sound_cid);
    if (!s)
        return JS_EXCEPTION;
    switch (s->kind * 8 + magic) {
    case RL_SOUND * 8 + 0:
        SetSoundVolume(s->sound, v);
        break;
    case RL_SOUND * 8 + 1:
        SetSoundPitch(s->sound, v);
        break;
    case RL_SOUND * 8 + 2:
        SetSoundPan(s->sound, v);
        break;
    case RL_MUSIC * 8 + 0:
        SetMusicVolume(s->music, v);
        break;
    case RL_MUSIC * 8 + 1:
        SetMusicPitch(s->music, v);
        break;
    case RL_MUSIC * 8 + 2:
        SetMusicPan(s->music, v);
        break;
    case RL_MUSIC * 8 + 3:
        if (v > GetMusicTimeLength(s->music))
            return JS_ThrowRangeError(ctx, "dyna:raylib: seek past the end of the music");
        SeekMusicStream(s->music, v);
        break;
    case RL_MUSIC * 8 + 4:
        s->music.looping = on;
        break;
    case RL_STREAM * 8 + 0:
        SetAudioStreamVolume(s->stream, v);
        break;
    case RL_STREAM * 8 + 1:
        SetAudioStreamPitch(s->stream, v);
        break;
    case RL_STREAM * 8 + 2:
        SetAudioStreamPan(s->stream, v);
        break;
    default:
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s does not apply to this kind of audio", names[magic]);
    }
    return JS_UNDEFINED;
}

JSValue rl_stream_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int rate, ch;
    rl_snd* s;
    JSValue obj;
    if (rl_args(ctx, argc, argv, 2, "new AudioStream(sampleRate, channels)") || rl_int_in(ctx, argv[0], "AudioStream sampleRate", 8000, 192000, &rate)
        || rl_int_in(ctx, argv[1], "AudioStream channels", 1, 2, &ch))
        return JS_EXCEPTION;
    if (!rl_audio_ready(ctx, "new AudioStream()"))
        return JS_EXCEPTION;
    s = dyn_nat_calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->kind = RL_STREAM;
    s->channels = ch;
    s->stream = LoadAudioStream((unsigned)rate, 32, (unsigned)ch);
    if (!IsAudioStreamValid(s->stream)) {
        dyn_nat_free(s);
        return JS_ThrowInternalError(ctx, "dyna:raylib: the audio device refused a %d Hz %d-channel stream", rate, ch);
    }
    s->alive = 1;
    rl_snd_link(s);
    obj = dyn_res_wrap(ctx, new_target, rl_stream_cid, s, rl_snd_dispose);
    return obj;
}

JSValue rl_stream_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    size_t off, blen, bpe, ab;
    JSValue buf;
    uint8_t* base;
    rl_snd* s;
    if (argc < 1 || JS_GetClassID(argv[0]) != rl_f32_cid)
        return JS_ThrowTypeError(ctx, "dyna:raylib: update(samples) needs a Float32Array of interleaved samples");
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (!rl_audio_ready(ctx, "AudioStream#update()"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, rl_stream_cid);
    if (!s)
        return JS_EXCEPTION;
    if (off > ab || blen > ab - off || (blen / 4) % (size_t)s->channels || blen / 4 / (size_t)s->channels > INT32_MAX)
        return JS_ThrowRangeError(ctx, "dyna:raylib: update needs whole frames of %d interleaved samples", s->channels);
    if (!IsAudioStreamProcessed(s->stream))
        return JS_ThrowTypeError(ctx, "dyna:raylib: the stream has not played its last buffer yet (check processed first)");
    if (blen)
        UpdateAudioStream(s->stream, base + off, (int)(blen / 4 / (size_t)s->channels));
    return JS_UNDEFINED;
}

#endif
