#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <fcntl.h>
#include <jni.h>
#include "dyna-ui-backend.h"
#include <stdio.h>
#include <unistd.h>

int dyn_cli_main(int argc, char** argv);

JNIEXPORT void JNICALL Java_org_dynajs_app_DynaActivity_nativeKeyboardInset(JNIEnv* env, jclass cls, jint bottom)
{
    (void)env;
    (void)cls;
    SDL_SetFloatProperty(SDL_GetGlobalProperties(), DYN_UIB_KEYBOARD_PROP, bottom > 0 ? (float)bottom : 0);
}

static int dyn_android_extract(const char* asset, const char* dir, const char* name, char* path, size_t cap)
{
    size_t n = 0, done;
    void* data = SDL_LoadFile(asset, &n);
    FILE* f;
    if (!data)
        return -1;
    snprintf(path, cap, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        SDL_free(data);
        return -1;
    }
    done = fwrite(data, 1, n, f);
    SDL_free(data);
    return fclose(f) == 0 && done == n ? 0 : -1;
}

int main(int argc, char** argv)
{
    const char* dir = SDL_GetAndroidInternalStoragePath();
    static char name[] = "dynajs", std_flag[] = "--std", window_flag[] = "--window";
    char script[1024], log[1024];
    char* args[] = { name, std_flag, script, window_flag, NULL };
    int fd;
    (void)argc;
    (void)argv;
    if (!dir)
        return 1;
    snprintf(log, sizeof(log), "%s/dynajs.log", dir);
    fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (dyn_android_extract("dynajs-app.js", dir, "app.js", script, sizeof(script))) {
        fprintf(stderr, "dynajs: the dynajs-app.js asset could not be copied to %s: %s\n", dir, SDL_GetError());
        return 1;
    }
    if (chdir(dir) != 0)
        return 1;
    return dyn_cli_main(4, args);
}
