#include <stdio.h>

#include "windows.h"

#include "ffmpeg_dxva2.h"

#define __STDC_CONSTANT_MACROS

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavdevice/avdevice.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavfilter/avfilter.h"
#include "SDL2/SDL.h"
};


#define USE_HWACCEL 1

static FILE *output_file = NULL;

#if USE_HWACCEL
AVPixelFormat GetHwFormat(AVCodecContext *s, const AVPixelFormat *pix_fmts)
{
    InputStream* ist = (InputStream*)s->opaque;
    ist->active_hwaccel_id = HWACCEL_DXVA2;
    ist->hwaccel_pix_fmt = AV_PIX_FMT_DXVA2_VLD;
    return ist->hwaccel_pix_fmt;
}
#endif

// https://github.com/Microsoft/vcpkg/issues/641
#ifdef _DEBUG
#pragma comment(lib, "SDL2maind.lib")
#else
#pragma comment(lib, "SDL2main.lib")
#endif

int main(int argc, char* argv[])
{
    AVFormatContext	*pFormatCtx;
    int	i, videoindex;
    AVCodecContext	*pCodecCtx;
    AVCodec	*pCodec;
    AVFrame	*pFrame, *pFrameTarget;
    AVFrame *sw_frame = NULL;
    //uint8_t *out_buffer;
    AVPacket *packet;
    int y_size;
    int ret, got_picture;
    struct SwsContext *img_convert_ctx = nullptr;

    const char* filepath = (argc == 2)? argv[1] : "e:/1.MP4";
    //SDL---------------------------
    int screen_w = 0, screen_h = 0;
    SDL_Window *screen;
    SDL_Renderer* sdlRenderer;
    SDL_Texture* sdlTexture;

#if ( LIBAVFORMAT_VERSION_INT <= AV_VERSION_INT(58,9,100) )
    avcodec_register_all();
    av_register_all();
#endif
    avdevice_register_all();
    pFormatCtx = avformat_alloc_context();

    if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0)
    {
        printf("Couldn't open input stream.\n");
        return -1;
    }
    if (avformat_find_stream_info(pFormatCtx, NULL)<0) {
        printf("Couldn't find stream information.\n");
        return -1;
    }

    videoindex = -1;
    for (i = 0; i<pFormatCtx->nb_streams; i++)
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoindex = i;
            break;
        }
    if (videoindex == -1) {
        printf("Didn't find a video stream.\n");
        return -1;
    }
    auto videoStream = pFormatCtx->streams[videoindex];

    //pCodecCtx = pFormatCtx->streams[videoindex]->codec;
    pCodecCtx = avcodec_alloc_context3(nullptr);

    avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoindex]->codecpar);

    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (pCodec == NULL) {
        printf("Codec not found.\n");
        return -1;
    }
#if USE_HWACCEL

    pCodecCtx->coded_width = pCodecCtx->width;
    pCodecCtx->coded_height = pCodecCtx->height;

    pCodecCtx->thread_count = 1;  // Multithreading is apparently not compatible with hardware decoding

    ////Add HwAccel
    InputStream *ist = new InputStream();
    ist->hwaccel_id = HWACCEL_AUTO;
    //ist->hwaccel_device = "dxva2";
    ist->dec = pCodec;
    ist->dec_ctx = pCodecCtx;

    pCodecCtx->opaque = ist;
    if (dxva2_init(pCodecCtx) >= 0)
    {
        pCodecCtx->get_buffer2 = ist->hwaccel_get_buffer;
        pCodecCtx->get_format = GetHwFormat;
        pCodecCtx->thread_safe_callbacks = 1;
        //pCodecCtx->thread_count = 1;  // Multithreading is apparently not compatible with hardware decoding
    }
    else
    {
        delete ist;
        pCodecCtx->opaque = nullptr;
        pCodecCtx->thread_count = 2;
        pCodecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    }
#else
    pCodecCtx->opaque = nullptr;
    pCodecCtx->thread_count = 2;
    pCodecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
#endif // USE_HWACCEL
    if (avcodec_open2(pCodecCtx, pCodec, NULL)<0) {
        printf("Could not open codec.\n");
        return -1;
    }

    screen_w = pCodecCtx->width;
    screen_h = pCodecCtx->height;
    AVPixelFormat image_transf_format = AV_PIX_FMT_YUV420P; //AV_PIX_FMT_NV12;

    pFrame = av_frame_alloc();
    sw_frame = av_frame_alloc();

    pFrameTarget = av_frame_alloc();
    //out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(image_transf_format, screen_w, screen_h, 1));
    //av_image_fill_arrays(pFrameTarget->data, pFrameTarget->linesize, out_buffer,
    //    image_transf_format, screen_w, screen_h, 1);
    pFrameTarget->width = screen_w;
    pFrameTarget->height = screen_h;
    pFrameTarget->format = image_transf_format;
    av_frame_get_buffer(pFrameTarget, 16);

    packet = (AVPacket *)av_malloc(sizeof(AVPacket));
    //Output Info-----------------------------
    printf("--------------- File Information ----------------\n");
    av_dump_format(pFormatCtx, 0, filepath, 0);
    printf("-------------------------------------------------\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        printf("Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    ////SDL 2.0 Support for multiple windows
    screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        screen_w, screen_h,
        SDL_WINDOW_OPENGL);
    sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
    sdlTexture = SDL_CreateTexture(sdlRenderer, 
        SDL_PIXELFORMAT_YV12, //SDL_PIXELFORMAT_NV12,
        SDL_TEXTUREACCESS_STREAMING, 
        screen_w, screen_h);

    //SDL End----------------------
    bool quit = false;
    while (!quit && av_read_frame(pFormatCtx, packet) >= 0) {
        if (packet->stream_index == videoindex)
        {
            int nTime0 = GetTickCount();
            ret = avcodec_send_packet(pCodecCtx, packet);
            if (ret < 0)
                continue;
            ret = avcodec_receive_frame(pCodecCtx, pFrame);
            if (ret < 0) {
                printf("Decode Error.\n");
                continue;
            }

            if (pFrame->format == AV_PIX_FMT_DXVA2_VLD)
            {
                InputStream* ist = (InputStream *)pCodecCtx->opaque;
                int nTime0 = GetTickCount();
                ist->hwaccel_retrieve_data(pCodecCtx, pFrame);
                //sw_frame = pFrame;
                av_frame_move_ref(sw_frame, pFrame);
                printf("convert time :%d\n", GetTickCount() - nTime0);
                //sw_frame = pFrame;
            }
            else
            {
                sw_frame = pFrame;
            }
            if (1) {
                if (!img_convert_ctx)
                {
                    img_convert_ctx = sws_getContext(sw_frame->width, sw_frame->height, (AVPixelFormat)sw_frame->format,
                        pFrameTarget->width, pFrameTarget->height, image_transf_format, SWS_BICUBIC, NULL, NULL, NULL);
                }
                sws_scale(img_convert_ctx, (const uint8_t* const*)sw_frame->data, sw_frame->linesize, 0, sw_frame->height,
                    pFrameTarget->data, pFrameTarget->linesize);

                //SDL_UpdateTexture(sdlTexture, NULL, pFrameTarget->data[0], pFrameTarget->linesize[0]);
                SDL_UpdateYUVTexture(sdlTexture, nullptr,
                    pFrameTarget->data[0], pFrameTarget->linesize[0],
                    pFrameTarget->data[1], pFrameTarget->linesize[1],
                    pFrameTarget->data[2], pFrameTarget->linesize[2]);

                //SDL_RenderClear(sdlRenderer);
                SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
                SDL_RenderPresent(sdlRenderer);
            }
            SDL_Delay(40);
            av_frame_unref(sw_frame);
        }

        // https://stackoverflow.com/questions/48226816/sdl-window-freezes
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            /* handle your event here */

            //User requests quit
            if (event.type == SDL_QUIT)
                quit = true;
        }
    }
    sws_freeContext(img_convert_ctx);

    SDL_Quit();

    av_frame_free(&pFrameTarget);
    av_frame_free(&pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);

    return 0;
}