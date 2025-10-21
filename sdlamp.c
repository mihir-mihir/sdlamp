#include "SDL.h"

static SDL_AudioDeviceID audio_device = 0;
SDL_AudioSpec wavspec;
static SDL_Window* window = NULL;  // any time you need to draw to screen in sdl, you need a window
static SDL_Renderer* renderer = NULL;

#if defined(__GNUC__) || defined(__clang__)
static void panic_and_abort(const char* title, const char* text) __attribute__((noreturn));  // c/c++ attributes
#endif

static void panic_and_abort(const char* title, const char* text) {
    fprintf(stderr, "PANIC: %s ... %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
    SDL_Quit();
    exit(1);
}

// THIS GLOBAL STATE IS NOT PERMANAENT
// static variables in C are initialized to zero when declared
static Uint8* wavbuf = NULL;
static Uint32 wavlen = 0;
static Uint32 wavpos = 0; // current byte position in wav file
static SDL_AudioSpec wavspec;

static SDL_bool open_new_audio_file(char* fname) {
    SDL_FreeWAV(wavbuf);  // if you pass SDL_free a null it's a noop
    wavbuf = NULL;
    wavlen = 0;
    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file", SDL_GetError(), window);
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

int main() {
    Uint8* wavbuf = NULL;
    Uint32 wavlen = 0;
    SDL_AudioSpec wavspec;
    sdfsdf;
    SDL_AudioSpec desired;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    // tells sdl we want this event type enabled it's disabled by default bc dropfile event triggers
    // dynamic allocation of char* - memory will leak unless we free it explicitly with SDL_free when
    // we're done with it
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    open_new_audio_file("music.wav");

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    // renderer overlays things on created window - create opengl context?
    // talk to the render in simple primitives
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }

    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = NULL;
    // 2nd null arg is for obtained audiospec param, telling sdl that if hardware has issues with desired spec, make SDL
    // "fake" desired spec so that we can write code for desired spec (HAS to work with desired spec)
    audio_device = SDL_OpenAudioDevice(NULL, 0, &wavspec, NULL, 0);

    if (audio_device == 0) {
        panic_and_abort("Couldn't open audio device", SDL_GetError());
    } else {
        SDL_QueueAudio(audio_device, wavbuf, wavlen);
        SDL_PauseAudioDevice(audio_device, paused);
    }

    SDL_bool paused = SDL_TRUE;
    const SDL_Rect rewind_rect = {110, 100, 100, 100};
    const SDL_Rect pause_rect = {430, 100, 100, 100};

    int green = 0;
    SDL_bool keep_going = SDL_TRUE;
    while (keep_going) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: {
                keep_going = SDL_FALSE;
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                const SDL_Point pt = {e.button.x, e.button.y};
                if (SDL_PointInRect(&pt, &rewind_rect)) {
                    // need to make sure that audio device exists - it won't if no file
                    // was dropped or if there was a problem opening one s
                    if (audio_device) {
                        SDL_ClearQueuedAudio(audio_device);
                        SDL_QueueAudio(audio_device, wavbuf, wavlen);
                    }
                } else if (SDL_PointInRect(&pt, &pause_rect)) {
                    paused = paused ? SDL_FALSE : SDL_TRUE;
                    if (audio_device) {
                        SDL_PauseAudioDevice(audio_device, paused);
                    }
                }
                break;
            }

            case SDL_DROPFILE: {
                open_new_audio_file(e.drop.file);
                sdfsdf;
                SDL_free(e.drop.file);
                break;
            }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, green, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        SDL_RenderFillRect(renderer, &rewind_rect);
        SDL_RenderFillRect(renderer, &pause_rect);
        SDL_RenderPresent(renderer);

        green = (green + 1) % 256;
    }

    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}
