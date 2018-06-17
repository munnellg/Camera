#include <stdio.h>
#include <stdlib.h>

#include <errno.h>     /* perror */
#include <fcntl.h>     /* open */
#include <unistd.h>    /* close */
#include <memory.h>    /* memset */
#include <sys/mman.h>  /* mmap */
#include <sys/ioctl.h> /* ioctl */

#include <linux/videodev2.h>

#include <SDL2/SDL.h>

#define DEFAULT_SCREEN_WIDTH  800
#define DEFAULT_SCREEN_HEIGHT 600
#define DEFAULT_VIDEODEVICE   "/dev/video0"

#define APP_NAME "Camera"
#define NUMBUFS  16

struct state {
    /* camera properties */    
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers rb;
    struct v4l2_buffer buf;
    
    int   fd;
    void *mem[NUMBUFS];   

    /* screen properties */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    /* general properties */
    int width, height;       /* camera/screen resolution */
    int quit;                /* flag - 1 when program should quit */
};

struct args {
    char *videodevice;
    int   width, height;
    int   fullscreen;
};

static void
usage ( const char *progname ) {
    fprintf( stdout, "usage: %s [options]\n", progname );
    fprintf( stdout, "\n" );
    fprintf( stdout, "options:\n" );
    fprintf( stdout, "\t-d Path to video device\n" );
    fprintf( stdout, "\t-W Screen width\n" );
    fprintf( stdout, "\t-H Screen height\n" );
    fprintf( stdout, "\t-f Run in fullscreen mode\n" );
    fprintf( stdout, "\t-h Print this help message\n" );


    exit(0);
}

static void
parse_args ( struct args *args, int argc, char *argv[] ) {
    /* set up default values */
    args->videodevice = DEFAULT_VIDEODEVICE;
    args->width = DEFAULT_SCREEN_WIDTH;
    args->height = DEFAULT_SCREEN_HEIGHT;
    args->fullscreen = 0;

    /* get command line input */
    for ( int i = 1; i < argc; i++ ) {
        if ( argv[i][0] == '-' ) {
            /* found a flag - check what it means */
            switch ( argv[i][1] ) {
            case 'd': 
                args->videodevice = argv[++i]; 
                break;
            case 'W':
                args->width = atoi(argv[++i]);
                break;
            case 'H':
                args->height = atoi(argv[++i]);
                break;
            case 'f':
                args->fullscreen = 1;
                break;
            case 'h':
                usage(argv[0]);
            default:
                fprintf( stderr, "Unexpected flag : %s\n", argv[i] );
                break;
            }
        } else {
            /* program doesn't expect any arguments */
            fprintf( stderr, "Unexpected argument : %s\n", argv[i] ); 
        }
    }
}

static int
init ( struct state *s, struct args *a ) {
    /* zero everything in program state */
    memset(s, 0, sizeof(struct state));
    
    /* open camera file */
    s->fd = open(a->videodevice, O_RDWR);
    if ( s->fd < 0 ) { 
        perror(a->videodevice);
        return 0;
    }

    /* lets see what this camera can do... */
    if ( ioctl( s->fd, VIDIOC_QUERYCAP, &s->cap ) < 0 ) {
        fprintf( stderr, "Failed to open device : %s\n", a->videodevice );
        return 0;
    }

    if ( (s->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ) {
        fprintf( stderr, "%s does not support video capture\n", a->videodevice );
        return 0;
    }

    if ( (s->cap.capabilities & V4L2_CAP_STREAMING) == 0 ) {
        fprintf( stderr, "%s does not support streaming\n", a->videodevice );
        return 0;   
    }

    /* set up the camera's capture format */
    s->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s->fmt.fmt.pix.width = a->width;
    s->fmt.fmt.pix.height = a->height;    
    s->fmt.fmt.pix.field = V4L2_FIELD_ANY;
    /* I guess you should query this from cap? */
    s->fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; 

    if ( ioctl(s->fd, VIDIOC_S_FMT, s->fmt) < 0 ) {
        fprintf( stderr, "%s cannot set format\n", a->videodevice );
        return 0; 
    }

    /* Setting the format can succeed if the resolution is not supported. */
    /* This block checks for problems with resolution and updates accordingly */
    if ( s->fmt.fmt.pix.width != a->width ||
        s->fmt.fmt.pix.height != a->height ) {
        fprintf( stderr, "Requested resolution %dx%d is not available\n",
            a->width, a->height
        );
        fprintf( stderr, "Using resolution %dx%d\n",
            s->fmt.fmt.pix.width, s->fmt.fmt.pix.height
        );

        /* update settings resulution to reflect actual resolution */
        a->width = s->fmt.fmt.pix.width;
        a->height = s->fmt.fmt.pix.height;
    }

    /* record actual resolution in program state */
    s->width = a->width;
    s->height = a->height;

    /* set up how we will get data from camera (use memory mapping) */
    s->rb.count = NUMBUFS;
    s->rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s->rb.memory = V4L2_MEMORY_MMAP;

    if ( ioctl( s->fd, VIDIOC_REQBUFS, &s->rb) < 0 ) {
        fprintf( stderr, "Unable to allocate buffers : %d\n", errno );
        return 0;
    }

    /* map buffers */
    for ( int i=0; i<NUMBUFS; i++ ) {
        memset( &s->buf, 0, sizeof(struct v4l2_buffer) );
        s->buf.index = i;
        s->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        s->buf.memory = V4L2_MEMORY_MMAP;
        if ( ioctl( s->fd, VIDIOC_QUERYBUF, &s->buf) < 0 ) {
            fprintf( stderr, "Unable to query buffer %d\n", i );
            return 0;
        }

        s->mem[i] = mmap( 
            0, s->buf.length, PROT_READ, MAP_SHARED, s->fd, s->buf.m.offset 
        );

        if ( s->mem[i] == MAP_FAILED ) {
            fprintf (stderr, "Unable to map buffer %d\n", i);
            return 0;
        }
    }

    /* queue buffers */    
    for ( int i=0; i<NUMBUFS; i++ ) {
        memset( &s->buf, 0, sizeof(struct v4l2_buffer));
        s->buf.index = i;
        s->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        s->buf.memory = V4L2_MEMORY_MMAP;
        if ( ioctl( s->fd, VIDIOC_QBUF, &s->buf) < 0 ) {
            fprintf (stderr, "Unable to queue buffer %d\n", i);
            return 0;
        }
    }
    
    /* enable streaming from the camera */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( ioctl(s->fd, VIDIOC_STREAMON, &type) < 0 ) {
        fprintf( stderr, "Unable to start capture %d\n", errno);
        return 0;
    }

    /* initialize SDL which will be used for rendering */
    if ( SDL_Init( SDL_INIT_VIDEO ) < 0 ) {
        fprintf( stderr, "SDL_Init : %s\n", SDL_GetError() );
        return 0;
    }

    int stat = SDL_CreateWindowAndRenderer( 
        s->width, s->height, a->fullscreen * SDL_WINDOW_FULLSCREEN_DESKTOP,
        &s->window, &s->renderer
    );

    if ( stat < 0 ) {
        fprintf( stderr, "SDL_CreateWindowAndRenderer : %s\n", SDL_GetError() );
        return 0;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(s->renderer, s->width, s->height);
    SDL_SetWindowTitle(s->window, APP_NAME);

    /* Pixel format should match that of the camera for simplicity. */
    /* We're going to write pixels directly to texture so enable streaming. */
    s->texture =SDL_CreateTexture( 
        s->renderer, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING,
        s->width, s->height
    );

    return 1;
}

static void
handle_events ( struct state *s ) {
    SDL_Event e;

    while ( SDL_PollEvent(&e) ) {
        switch (e.type) {
        case SDL_QUIT:
            s->quit = 1;
            break;
        case SDL_KEYDOWN:
            if ( e.key.keysym.sym == SDLK_q ) { s->quit = 1; }
            break;     
        }
    }
}

static void
render ( struct state *s ) {
    void *pixels;
    int pitch;

    /* dequeue the next frame from the camera */
    memset(&s->buf, 0, sizeof(struct v4l2_buffer));
    s->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s->buf.memory = V4L2_MEMORY_MMAP;
    if ( ioctl(s->fd, VIDIOC_DQBUF, &s->buf) < 0 ) {
        fprintf( stderr, "Failed to dequeue buffer %d\n", errno );
        return;
    }

    SDL_LockTexture( s->texture, NULL, &pixels, &pitch );
    
    /* FIXME: Should be better behaviour to handle pixels size != 2 bytes */
    if ( pitch/s->width != sizeof(Uint16) ) {
        fprintf( stderr, "mismatch between texture size and buffer size\n" );
    } else {
        /* copy camera buffer over to texture */ 
        memcpy( 
            pixels, s->mem[s->buf.index], s->width*s->height*sizeof(Uint16) 
        );
    }

    SDL_UnlockTexture( s->texture );

    /* queue next frame for this buffer */
    if ( ioctl( s->fd, VIDIOC_QBUF, &s->buf ) < 0 ) {
        fprintf( stderr, "Failed to requeue buffer %d\n", errno );
    }

    /* update screen and present texture */
    SDL_RenderClear(s->renderer);
    SDL_RenderCopy(s->renderer, s->texture, NULL, NULL);
    SDL_RenderPresent(s->renderer);
}

static void
quit ( struct state *s ) {
    /* disable streaming from the camera */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( ioctl(s->fd, VIDIOC_STREAMOFF, &type) < 0 ) {
        fprintf( stderr, "Unable to stop capture %d\n", errno);
    }

    /* unmap all the buffers used for storing camera frames */
    for ( int i=0; i<NUMBUFS; i++ ) {
        munmap( s->mem[i], s->buf.length);
    }

    /* close file descriptor for the camera */
    if (s->fd) { close(s->fd); }

    /* release SDL resources */
    if (s->texture)  { SDL_DestroyTexture(s->texture); }
    if (s->renderer) { SDL_DestroyRenderer(s->renderer); }
    if (s->window)   { SDL_DestroyWindow(s->window); }
    SDL_Quit();
}

int
main ( int argc, char *argv[] ) {
    struct state state;
    struct args  args;
    
    /* get command line args */
    parse_args(&args, argc, argv);

    /* initialize program and quit if anything goes wrong */
    if ( init(&state, &args) == 0 ) {
        quit(&state);
        return EXIT_FAILURE;
    }

    /* run the program until the user quits */
    while ( state.quit == 0 ) {
        handle_events(&state);
        render(&state);
    }

    /* shutdown everything */
    quit(&state);

    return EXIT_SUCCESS;
}
