/*
NOTES FOR POST 16:20 AUDIO STREAMS VIDEO
-----------------------------------------
- general idea: implementing volume control involves making changes to the audio data in real time
- going to feed audio device a little bit at a time instead of dumping the entire audio file in the queue
- manipulating the audio will get complicated if allowed to use a lot of different formats,
    fix a single format to work with
- SDL_GetQueuedAudioSize -- how many bytes have I given to SDL to give to the audio device by calling SDL_QueueAudio
- if less than 8 kB that we've given, need to feed more data (give 32 kB at a time for now? or however much is left if
less)
- SDL_assert
- rewind button logic (clear queue, update wavpos), pause button logic
- SDL_AudioStream global - init to NULL to be explicit
    - this is how we "fake" desired audiospec?
    - now only the stream needs to be global

NOTES FOR VOLUME CONTROL VIDEO
--------------------------------
- sdl mouse motion event
- sample frames - 2 samples make up a frame in stereo, we are just going to work on all samples without worrying about
frames
- in converted_buf, each sample is a float (4 bytes) - remember converted_buf is type Uint8* though, so need to cast to
float* to go sample by sample in for loop


NOTES FOR BALANCE CONTROL VIDEO (5)
------------------------------------
- goto failed (gotos in c vs c++)
- stop audio instead of exiting in some cases?

NOTES FOR WINAMP INTRO VIDEO
-----------------------------
- surfaces cpu, textures gpu, utility for creating texture from surface (then free the surface)
- SDL_RenderCopy
- create load_texture subroutine

NOTES FOR REFACTOR VIDEO
-------------------------
- using pointer to winampskin in load_skin method bc you can't use references (&) in C,
    only way to "pass by reference" is with pointers
- SDL_arraysize macro
- draw_frame(), draw_button()
- converted_buffer array is static bc in old times was poor practice to put 2kB on the stack
*/

#include "SDL.h"

typedef struct {
    SDL_Texture* tex;
    SDL_Rect src_unpressed;
    SDL_Rect src_pressed;
    SDL_Rect dest;
    SDL_bool pressed;
} WinampSkinBtn;

typedef enum { BTN_PREV, BTN_PLAY, BTN_PAUSE, BTN_STOP, BTN_NEXT, BTN_EJECT, BTN_TOTAL } WinampSkinBtnID;

typedef struct {
    SDL_Texture* tex_main;
    SDL_Texture* tex_cbuttons;
    WinampSkinBtn buttons[BTN_TOTAL];
} WinampSkin;

static SDL_AudioDeviceID audio_device = 0;
SDL_AudioSpec wavspec;
static SDL_Window* window = NULL;  // any time you need to draw to screen in sdl, you need a window
static SDL_Renderer* renderer = NULL;

// THIS GLOBAL STATE IS NOT PERMANAENT
// static variables in C are initialized to zero when declared
// eventually might want to put these in a struct so one "thing" is getting passed around, not a lot of individual
// globals

static SDL_AudioStream* stream = NULL;  // don't have to initialize stack variable to null?
static SDL_AudioSpec desired;
static Uint8* wavbuf = NULL;
static Uint32 wavlen = 0;

static float volume_level = 1.0f;

static WinampSkin skin;

static SDL_bool paused = SDL_TRUE;

#if defined(__GNUC__) || defined(__clang__)
static void panic_and_abort(const char* title, const char* text) __attribute__((noreturn));  // c/c++ attributes
#endif

static void panic_and_abort(const char* title, const char* text) {
    fprintf(stderr, "PANIC: %s ... %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
    SDL_Quit();
    exit(1);
}

// inlined funtion?
static SDL_INLINE void init_skin_btn(
        WinampSkinBtn* btn,
        SDL_Texture* tex,
        const SDL_Rect src_unpressed,
        const SDL_Rect src_pressed,
        const SDL_Rect dest) {
    btn->tex = tex;
    btn->src_unpressed = src_unpressed;
    btn->src_pressed = src_pressed;
    btn->dest = dest;
    btn->pressed = SDL_FALSE;
}

static SDL_Texture* load_texture(const char* fname) {
    SDL_Surface* surface = SDL_LoadBMP(fname);
    if (!surface) {
        return NULL;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;  // may be NULL
}

static SDL_bool load_skin(WinampSkin* skin) {  // FIXME: use fname var
    SDL_zerop(skin);  // zerop lets you pass in pointer instead of dereferenced ptr

    skin->tex_main = load_texture("old_macos_skin/Main.bmp");
    skin->tex_cbuttons = load_texture("old_macos_skin/CButtons.bmp");

    const SDL_Rect prev = {16, 88, 23, 18};
    const SDL_Rect play = {39, 88, 23, 18};
    const SDL_Rect pause = {62, 88, 23, 18};
    const SDL_Rect stop = {85, 88, 23, 18};
    const SDL_Rect next = {108, 88, 22, 18};
    const SDL_Rect eject = {136, 89, 22, 16};

    const SDL_Rect cbtns_prev_unpressed = {0, 0, 23, 18};
    const SDL_Rect cbtns_play_unpressed = {23, 0, 23, 18};
    const SDL_Rect cbtns_pause_unpressed = {46, 0, 23, 18};
    const SDL_Rect cbtns_stop_unpressed = {69, 0, 23, 18};
    const SDL_Rect cbtns_next_unpressed = {92, 0, 22, 18};
    const SDL_Rect cbtns_eject_unpressed = {114, 0, 22, 16};

    const SDL_Rect cbtns_prev_pressed = {0, 18, 23, 18};
    const SDL_Rect cbtns_play_pressed = {23, 18, 23, 18};
    const SDL_Rect cbtns_pause_pressed = {46, 18, 23, 18};
    const SDL_Rect cbtns_stop_pressed = {69, 18, 23, 18};
    const SDL_Rect cbtns_next_pressed = {92, 18, 22, 18};
    const SDL_Rect cbtns_eject_pressed = {114, 16, 22, 16};

    init_skin_btn(&(skin->buttons[BTN_PREV]), skin->tex_cbuttons, cbtns_prev_unpressed, cbtns_prev_pressed, prev);
    init_skin_btn(&(skin->buttons[BTN_PLAY]), skin->tex_cbuttons, cbtns_play_unpressed, cbtns_play_pressed, play);
    init_skin_btn(&(skin->buttons[BTN_PAUSE]), skin->tex_cbuttons, cbtns_pause_unpressed, cbtns_pause_pressed, pause);
    init_skin_btn(&(skin->buttons[BTN_STOP]), skin->tex_cbuttons, cbtns_stop_unpressed, cbtns_stop_pressed, stop);
    init_skin_btn(&(skin->buttons[BTN_NEXT]), skin->tex_cbuttons, cbtns_next_unpressed, cbtns_next_pressed, next);
    init_skin_btn(&(skin->buttons[BTN_EJECT]), skin->tex_cbuttons, cbtns_eject_unpressed, cbtns_eject_pressed, eject);

    return SDL_TRUE;
}

static void load_wav_to_stream(char* fname) {
    SDL_AudioSpec wavspec;

    SDL_FreeWAV(wavbuf);  // if you pass SDL_free a null it's a noop
    wavbuf = NULL;
    wavlen = 0;  // length in BYTES

    if (SDL_LoadWAV(fname, &wavspec, &wavbuf, &wavlen) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't load wav file", SDL_GetError(), window);
    }

    // free the stream pointer first in case it already exists so you don't leak memory
    SDL_FreeAudioStream(stream);
    stream = SDL_NewAudioStream(
            wavspec.format, wavspec.channels, wavspec.freq, desired.format, desired.channels, desired.freq);
    // error condition can happen if you run out of memory
    if (SDL_AudioStreamPut(stream, wavbuf, wavlen) == -1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't put data in audio stream", SDL_GetError(), window);
        // return SDL_FALSE;
        // maybe panic and abort here? bc you want to exit if you run out of memory
    }
    // !!! FIXME: error handling
    SDL_AudioStreamFlush(stream);  // convert all of the  data so it's available with SDL_AudioStreamGet
}
// FIXME!!! can have this function return an audio stream and get rid of the stream global?

static void init_everything() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1) {
        panic_and_abort("SDL_Init failed", SDL_GetError());
    }

    // tells sdl we want this event type enabled it's disabled by default bc dropfile event triggers
    // dynamic allocation of char* - memory will leak unless we free it explicitly with SDL_free when
    // we're done with it
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    window = SDL_CreateWindow("Hello SDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 275, 116, 0);
    if (!window) {
        panic_and_abort("SDL_CreateWindow failed", SDL_GetError());
    }

    // renderer overlays things on created window - create opengl context?
    // talk to the render in simple primitives
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        panic_and_abort("SDL_CreateRenderer failed", SDL_GetError());
    }
    // !!! FIXME: load real skin
    if (!load_skin(&skin)) {
        panic_and_abort("failed to load initial skin", SDL_GetError());
    }
    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_F32;
    desired.channels = 2;
    desired.samples = 4096;
    desired.callback = NULL;

    // 2nd null arg is for obtained audiospec param, telling sdl that if hardware has issues with desired spec, make SDL
    // "fake" desired spec so that we can write code for desired spec (HAS to work with desired spec)
    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (audio_device == 0) {
        panic_and_abort("Couldn't open audio device", SDL_GetError());
    }

    load_wav_to_stream("music.wav");
}

static void deinit_everything() {
    // !!! FIXME: free(skin)
    SDL_FreeWAV(wavbuf);
    SDL_CloseAudioDevice(audio_device);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();
}

static void feed_audio_to_device() {
    // in the video there's a var new_bytes = SDL_min(SDL_AudioStreamAvailable(stream), sizeof(converted_buf))
    // might not need this var though bc len parameter in SDL_AudioStreamGet is specified as "max bytes to fill",
    // not exact bytes to fill, so if there are fewer bytes remaining in the stream than the number of bytes in the
    // converted buffer, it should still be ok to give sizeof(converted_buf) as the arg for the len param of
    // SDL_AudioStreamGet (good to know about SDL_min function though)

    // it looks like we do need to use "gotten_bytes" to pass into SDL_QueueAudio function

    // 8MB stack on linux?
    static Uint8 converted_buf[32 * 1024];
    if (SDL_AudioStreamAvailable(stream) == 0) {
        return;
    }
    int gotten_bytes = SDL_AudioStreamGet(stream, converted_buf, sizeof(converted_buf));

    if (gotten_bytes == -1) {
        panic_and_abort("failed to get converted data", SDL_GetError());
    }

    float* converted_samples = (float*)converted_buf;
    int nsamples = gotten_bytes / sizeof(float);
    // changing volume based on volume_level here:
    for (int i = 0; i < nsamples; i++) {
        converted_samples[i] *= volume_level;
    }
    SDL_QueueAudio(audio_device, converted_buf, gotten_bytes);
}

static void draw_button(SDL_Renderer* renderer, WinampSkinBtn* btn) {
    if (btn->tex == NULL) {
        if (btn->pressed) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        }
        SDL_RenderFillRect(renderer, &(btn->dest));
    } else {
        SDL_RenderCopy(renderer, btn->tex, btn->pressed ? &(btn->src_pressed) : &(btn->src_unpressed), &(btn->dest));
    }
}
static void draw_frame(SDL_Renderer* renderer, WinampSkin* skin) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, skin->tex_main, NULL, NULL);

    for (unsigned long i = 0; i < SDL_arraysize(skin->buttons); i++) {
        draw_button(renderer, &skin->buttons[i]);
    }

    SDL_RenderPresent(renderer);

    // green = (green + 1) % 256;
}

static SDL_bool handle_events(WinampSkin* skin) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: {
                return SDL_FALSE;
                break;
            }
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEBUTTONDOWN: {
                const SDL_bool pressed = (e.button.state == SDL_PRESSED) ? SDL_TRUE : SDL_FALSE;
                const SDL_Point pt = {e.button.x, e.button.y};

                for (unsigned long i = 0; i < SDL_arraysize(skin->buttons); i++) {
                    WinampSkinBtn* btn = &skin->buttons[i];
                    btn->pressed = pressed && SDL_PointInRect(&pt, &btn->dest);
                    if (btn->pressed) {
                        switch ((WinampSkinBtnID)i) {
                            case BTN_PREV:
                                // !!! FIXME: if you spam restart button takes a while after the most recent press
                                // to
                                // play the audio - think this is because of multiple calls to convert the entire
                                // wav file
                                SDL_ClearQueuedAudio(audio_device);
                                SDL_AudioStreamClear(stream);
                                SDL_AudioStreamPut(stream, wavbuf, wavlen);

                                break;
                            case BTN_PAUSE:
                                paused = paused ? SDL_FALSE : SDL_TRUE;
                                SDL_PauseAudioDevice(audio_device, paused);
                                break;
                            default:
                                break;
                        }
                    }
                }
                break;
            }
                // case SDL_MOUSEMOTION: {
                //     const SDL_Point pt = {e.motion.x, e.motion.y};
                //     if (e.motion.state == SDL_PRESSED && SDL_PointInRect(&pt, &volume_rect)) {
                //         volume_level = (float)(e.motion.x - volume_rect.x) / volume_rect.w;
                //         volume_knob.x = pt.x - (volume_knob.w / 2);
                //         printf("mouse motion at (%d, %d), percent: %f\n", e.motion.x, e.motion.y, volume_level);
                //     }
                //     break;  // without break here, i get bad access exception on loadwav above - running out of
                //     memory?
                // }

            case SDL_DROPFILE: {
                load_wav_to_stream(e.drop.file);
                SDL_free(e.drop.file);
                break;
            }
        }
    }
    return SDL_TRUE;
}

int main() {
    init_everything();  // will panic and abort on fail
    while(handle_events(&skin)) {
        draw_frame(renderer, &skin);
        int queued_bytes = SDL_GetQueuedAudioSize(audio_device);
        if (queued_bytes < 8192) {
            feed_audio_to_device();
        }
    }

    deinit_everything();
    return 0;
}

// feed audio samples/data to the audio device
// load in wav at startup and when new file is selected (drag n drop for now)
// have audio play from device based on state of pause button
// restart current file when restart button pressed (will play or be paused based on pause button state
