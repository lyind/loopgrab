/**
 *  Driver for better https://looptap.vasanthv.com/ scores
 *
 *  author: lyind
 * 
 *  1. find game field by color
 *  2. find ball rect by color + shape
 *  3. check if ball rect is still surrounded by some field pixels
 *    -> if not: fire
 */
#include <cstdint>
#include <ctime>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>

#include <unistd.h>
#include <png.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>


using namespace std;


struct Pixel
{
    uint32_t c;

    Pixel() :
        c(0)
    {
    }

    Pixel(uint8_t b, uint8_t g, uint8_t r, uint8_t a) :
        c((((uint32_t)a) << 24) | (((uint32_t)r) << 16) | (((uint32_t)g) << 8) | (uint32_t)b)
    {
    }

    // from little-endian BGRA
    Pixel(const uint32_t bgra) :
        c(bgra)
    {
    }

    uint8_t r() const
    {
        return (uint8_t)((c >> 16) & 0xffu);
    }

    uint8_t g() const
    {
        return (uint8_t)((c >> 8) & 0xffu);
    }

    uint8_t b() const
    {
        return (uint8_t)(c & 0xffu);
    }

    uint8_t a() const
    {
        return (uint8_t)(c >> 24);
    }

    friend bool operator== (const Pixel& p1, const Pixel& p2);
    friend bool operator!= (const Pixel& p1, const Pixel& p2);
};

bool operator== (const Pixel& p1, const Pixel& p2)
{
    return p1.c == p2.c;
}

bool operator!= (const Pixel& p1, const Pixel& p2)
{
    return p1.c != p2.c;
}

ostream & operator<<(ostream &os, const Pixel& p)
{
    return os << "#"
        << setfill('0') << setw(sizeof(uint8_t)*2) << hex << (int)p.r()
        << setfill('0') << setw(sizeof(uint8_t)*2) << hex << (int)p.g()
        << setfill('0') << setw(sizeof(uint8_t)*2) << hex << (int)p.b()
        << setfill('0') << setw(sizeof(uint8_t)*2) << hex << (int)p.a();
}


struct Rect
{
    int x0;
    int y0;
    int x1;
    int y1;

    Rect() :
        x0(0),
        y0(0),
        x1(0),
        y1(0)
    {
    }

    Rect(int x0, int y0, int x1, int y1) :
        x0(x0),
        y0(y0),
        x1(x1),
        y1(y1)
    {
    }

    int width() const
    {
        return x1 - x0;
    }

    int height() const
    {
        return y1 - y0;
    }

    int centerX() const 
    {
        return x0 + (width() / 2);
    }

    int centerY() const 
    {
        return y0 + (height() / 2);
    }

    bool contains(int x, int y) const
    {
        return x >= x0 && x < x1
            && y >= y0 && y < y1;
    }

    void add(const Rect& other)
    {
        x0 = min(other.x0, x0);
        y0 = min(other.y0, y0);
        x1 = max(other.x1, x1);
        y1 = max(other.y1, y1);
    }

    friend bool operator== (const Rect& r1, const Rect& r2);
    friend bool operator!= (const Rect& r1, const Rect& r2);
};

bool operator== (const Rect& r1, const Rect& r2)
{
    return r1.x0 == r2.x0 && r1.y0 == r2.y0 && r1.x1 == r2.x1 && r1.y1 == r2.y1;
}

bool operator!= (const Rect& r1, const Rect& r2)
{
    return r1.x0 != r2.x0 || r1.y0 != r2.y0 || r1.x1 != r2.x1 || r1.y1 != r2.y1;
}

ostream & operator<<(ostream &os, const Rect& r)
{
    return os << "{{" << r.x0 << ", " << r.y0 << "}, {" << r.x1 << ", " << r.y1 << "}}";
}


class GameFrame
{
public:

    virtual void next() = 0;

    virtual Pixel getPixel(int x, int y) const = 0;

    virtual void savePng(const string path) const = 0;
};

class GameControls
{
public:

    virtual void fire() = 0;

    virtual void move(int x, int y) = 0;

    virtual void click(int x, int y) = 0;

    virtual void focus(int x, int y) = 0;
};


class Game
{
    const Pixel fieldColor = {0xf6, 0xf9, 0xfb, 0x00};
    const Pixel ballColor = {0x51, 0x3d, 0x2c, 0x00};

    GameControls& controls;
    const int width;
    const int height;
    Rect screen;
    Rect ball;
    Rect field;
    int frameCount;
    int lastFire;
    time_t lastBall;
    time_t lastBallMove;
    int deadzoneFrames;
    int ignoredCount;
    bool hasFired;

    void limit(Rect& r)
    {
        r.x0 = min(r.x1, min(width, max(0, r.x0)));
        r.y0 = min(r.y1, min(height, max(0, r.y0)));
        r.x1 = max(r.x0, min(width, max(0, r.x1)));
        r.y1 = max(r.y0, min(height, max(0, r.y1)));
    }

    bool shrink(Rect& r)
    {
        r.x0 = r.x0 + 1;
        r.y0 = r.y0 + 1;
        r.x1 = r.x1 - 1;
        r.y1 = r.y1 - 1;

        limit(r);

        return r.x0 != r.x1 || r.y0 != r.y1;
    }

    bool expand(Rect& r)
    {
        r.x0 = r.x0 - 1;
        r.y0 = r.y0 - 1;
        r.x1 = r.x1 + 1;
        r.y1 = r.y1 + 1;

        limit(r);

        return r.x0 != 0 || r.y0 != 0 || r.x1 != width || r.y1 != height;
    }

    bool topContainsColor(const GameFrame& frame, const Rect& r, const Pixel& color)
    {
        // top
        for (int x = r.x0; x < r.x1; ++x)
        {
            if (frame.getPixel(x, r.y0) == color)
            {
                return true;
            }
        }
        return false;
    }

    bool bottomContainsColor(const GameFrame& frame, const Rect& r, const Pixel& color)
    {
        // bottom
        for (int x = r.x0; x < r.x1; ++x)
        {
            if (frame.getPixel(x, r.y1) == color)
            {
                return true;
            }
        }
        return false;
    }

    bool leftContainsColor(const GameFrame& frame, const Rect& r, const Pixel& color)
    {
        // left
        for (int y = r.y0; y < r.y1; ++y)
        {
            if (frame.getPixel(r.x0, y) == color)
            {
                return true;
            }
        }

        return false;
    }

    bool rightContainsColor(const GameFrame& frame, const Rect& r, const Pixel& color)
    {
        // right
        for (int y = r.y0; y < r.y1; ++y)
        {
            if (frame.getPixel(r.x1, y) == color)
            {
                return true;
            }
        }
        return false;
    }

    bool findColorBounds(const GameFrame& frame, Rect& rect, const Pixel& color)
    {
        Rect r1 = rect;
        Rect r0;
        do
        {
            r0 = r1;

            // -x0
            r1.x0 = max(0, r1.x0 - 1);
            if (!leftContainsColor(frame, r1, color))
            {
                r1.x0 = r0.x0;
            }

            // -y0
            r1.y0 = max(0, r1.y0 - 1);
            if (!topContainsColor(frame, r1, color))
            {
                r1.y0 = r0.y0;
            }

            // +x1
            r1.x1 = min(width - 1, r1.x1 + 1);
            if (!rightContainsColor(frame, r1, color))
            {
                r1.x1 = r0.x1;
            }

            // +y1
            r1.y1 = min(height - 1, r1.y1 + 1);
            if (!bottomContainsColor(frame, r1, color))
            {
                r1.y1 = r0.y1;
            }

            //cerr << "findColorBounds(): r0=" << r0 << ", r1=" << r1 << endl;
        }
        while (r0 != r1);

        if (rect != r0)
        {
            rect = r0;
            return true;
        }

        return false;
    }

    bool isBallSurrounded(const GameFrame& frame, const Rect& ball)
    {
        Rect f = ball;

        // need ball frame (2 pixels outside for safety)
        expand(f); 
        expand(f);
        expand(f);

        return frame.getPixel(f.centerX(), f.y0) != fieldColor
            && frame.getPixel(f.centerX(), f.y1) != fieldColor
            && frame.getPixel(f.x0, f.centerY()) != fieldColor
            && frame.getPixel(f.x1, f.centerY()) != fieldColor;
    }

    bool checkForBall(const GameFrame& frame, int x, int y, Rect& ball)
    {
        if (frame.getPixel(x, y) != ballColor)
        {
            return false;
        }

        ball.x0 = x;
        ball.y0 = y;
        ball.x1 = x + 1;
        ball.y1 = y + 1;;
        if (findColorBounds(frame, ball, ballColor))
        {
            // a square and at least 3x3?
            int width = ball.width();
            int height = ball.height();
            if (width > 4 && height > 4 && width == height)
            {
                // corners must not be ball color (it is round)
                // while middle of edges must be
                if (frame.getPixel(ball.x0, ball.y0) != ballColor
                    && frame.getPixel(ball.x1, ball.y0) != ballColor
                    && frame.getPixel(ball.x0, ball.y1) != ballColor
                    && frame.getPixel(ball.x1, ball.y1) != ballColor
                    && frame.getPixel(ball.x0 + (width / 2), ball.y0 + 1) == ballColor
                    && frame.getPixel(ball.x0 + (width / 2), ball.y1 - 1) == ballColor
                    && frame.getPixel(ball.x0 + 1, ball.y0 + (height / 2)) == ballColor
                    && frame.getPixel(ball.x1 - 1, ball.y0 + (height / 2)) == ballColor)
                {
                    // is ball
                    return true;
                }
            }
        }

        return false;
    }

    bool findBall(const GameFrame& frame, const Rect& zone)
    {
        Rect b = ball;
        if (checkForBall(frame, b.centerX(), b.y0, b)
            || checkForBall(frame, b.x0, b.centerY(), b)
            || checkForBall(frame, b.centerX(), b.y1, b)
            || checkForBall(frame, b.x1, b.centerY(), b))
        {
            const time_t t = time(NULL);
            if (ball != b)
            {
                //cerr << "ball follow: " << b << endl;
                lastBallMove = t;
            }
            else
            {
                //cerr << "ball stuck: " << b << endl;
            }
            ball = b;
            lastBall = t;
            return true;
        }

        // try to find ball in zone
        b = ball;
        for (int y = zone.y0; y < zone.y1; y += max(1, ball.height() / 2))
        {
            for (int x = zone.x0; x < zone.x1; x += max(1, ball.width() / 2))
            {
                if (!b.contains(x, y) && checkForBall(frame, x, y, b))
                {
                    // cerr << "ball new: " << ball << endl;
                    ball = b;
                    const time_t t = time(NULL);
                    lastBall = t;
                    lastBallMove = t;
                    return true;
                }
            }
        }

        return false;
    }

    // did we already identify the playing field boundaries for this game?
    bool haveField() const
    {
        int unit = max(4, ball.width());
        return field.width() > unit * 10
            && field.height() > unit * 10
            && field.width() < field.height() + (unit / 4)
            && field.width() > field.height() - (unit / 4);
    }

    bool expandField(const GameFrame& frame)
    {
        // while determining field, always search whole screen
        if (!findBall(frame, Rect(0, 0, width, height)))
        {
            if (field.x1 == 0)
            {
                cerr << "error: no ball found: proof saved to no-ball-proof.png" << endl;
                frame.savePng("no-ball-proof.png");
                return false;
            }
            return true;
        }

        if (field.x1 == 0)
        {
            // initialize
            field = ball;
        }
        else
        {
            // add current ball to field (search area)
            field.add(ball);
        }

        return true;
    }

    void addFieldSafetyMargin()
    {
        for (int i = 0; i < ball.width(); ++i)
        {
            expand(field);
        }
    }

    // simulate keypress
    bool fire()
    {
        if (frameCount - lastFire >= deadzoneFrames)
        {
            controls.fire();
            cerr << "[" << frameCount << "] (ign. " << ignoredCount << ") FIRE!" << endl;
            ignoredCount = 0;
            lastFire = frameCount;
            return true;
        }
        else
        {
            ++ignoredCount;
            return false;
        }
    }


public:

    Game(GameControls& controls, int width, int height, int deadzoneFrames) :
        controls(controls),
        width(width),
        height(height),
        screen(0, 0, width, height),
        ball(0, 0, 0, 0),
        field(0, 0, 0, 0),
        frameCount(0),
        lastFire(0),
        lastBall(time(NULL)),
        lastBallMove(time(NULL)),
        deadzoneFrames(deadzoneFrames),
        ignoredCount(0),
        hasFired(false)
    {
    }

    ~Game()
    {
    }

    bool step(GameFrame& frame)
    {
        bool hadField = haveField();

        bool keepPlaying; 
        if (!hadField)
        {
            frame.next();

            const Rect lastField = field;
            keepPlaying = expandField(frame);
            if (keepPlaying)
            {
                if (lastField == field && field == ball)
                {
                    // initial space keypress to start game (if ball doesn't move)
                    controls.focus(ball.centerX(), ball.centerY());
                    controls.fire();
                    expand(field);
                    cerr << "[" << frameCount << "] game started" << endl;
                }
                else if (haveField())
                {
                    // finally add safety margin to the playing field
                    addFieldSafetyMargin();
                    controls.move(field.x1, field.y1);  // move away to not block view
                    cerr << "[" << frameCount << "] game field: " << field << endl;
                }
            }
        }
        else
        {
            frame.next();

            if (findBall(frame, field))
            {
                Rect ballBox = ball;
                expand(ballBox);
                if (isBallSurrounded(frame, ballBox))
                {
                    if (!hasFired)
                    {
                        // ball surrounded by non-field color: consider firing
                        hasFired = fire();
                    }
                }
                else
                {
                    hasFired = false;
                }

                // timeout after 2 s of ball not moving
                keepPlaying = time(NULL) - lastBallMove < 2;
                if (!keepPlaying)
                {
                    cerr << "[" << frameCount << "] game stopped" << endl;
                }
            }
            else
            {
                // timeout after 2s of not seeing any ball
                keepPlaying = time(NULL) - lastBall < 2;
            }
        }

        ++frameCount;

        return keepPlaying;
    }
};


class XImageFrame : public GameFrame
{
    Display *display;
    Window root;

    XImage *image;
    XShmSegmentInfo shminfo;
    int completionType;

public:

    XImageFrame(Display *display, Window root) :
        display(display),
        root(root),
        completionType(XShmGetEventBase(display) + ShmCompletion)
    {
        auto shm_ext = XInitExtension(display, "MIT-SHM");
        if (!shm_ext->extension)
        {
            cerr << "error: MIT-SHM extension not available" << endl;
        }

        XWindowAttributes root_attr;
        XGetWindowAttributes(display, root, &root_attr);

        image = XShmCreateImage(display, DefaultVisualOfScreen(root_attr.screen), root_attr.depth, ZPixmap, NULL, &shminfo, root_attr.width, root_attr.height); 

        shminfo.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT|0777);
        shminfo.shmaddr = image->data = (char*) shmat(shminfo.shmid, 0, 0);
        shminfo.readOnly = False;

        auto stat = XShmAttach(display, &shminfo);
        if (!stat)
        {
            cerr << "error: XShmAttach() failed" << endl;
        }

        //cerr << "ext=" << shm_ext->extension << ", shmid=" << shminfo.shmid << ", shmaddr=" << (uintptr_t)shminfo.shmaddr << ", vis=" << (uintptr_t)root_attr.visual << ", depth=" << root_attr.depth << ", width=" << root_attr.width << ", height=" << root_attr.height << ", stat=" << stat << ", completionType=" << completionType << endl;
    }

    ~XImageFrame()
    {
        XShmDetach(display, &shminfo);
        XDestroyImage(image);
        shmdt(shminfo.shmaddr);
        shmctl(shminfo.shmid, IPC_RMID, NULL);
    }


    void next()
    {
        auto success = XShmGetImage(display, root, image, 0, 0, AllPlanes);
        if (!success)
        {
            cerr << "error: XShmGetImage() failed" << endl;
        }

        //image = XShmGetImage(display, root, field.x0, field.y0, field.width(), field.height(), AllPlanes, XYPixmap);
        //cerr << "image: data=" << (uintptr_t)image->data
        //    << ", byte_order=" << image->byte_order
        //    << ", depth=" << image->depth
        //    << ", bytes_per_line=" << image->bytes_per_line
        //    << ", bits_per_pixel=" << image->bits_per_pixel
        //    << endl;
    }


    Pixel getPixel(int x, int y) const
    {
        return Pixel(image->f.get_pixel(image, x, y));
    }


    void savePng(const string path) const
    {
        FILE *file = fopen(path.c_str(), "wb");
        if (file == nullptr)
        {
            cerr << "error: failed to save PNG to " << path << ": " << strerror(errno) << endl;
            return;
        }

        png_image png;
        memset(&png, 0, sizeof(png));
        png.version = PNG_IMAGE_VERSION;
        png.width = image->width;
        png.height = image->height;
        png.format = PNG_FORMAT_BGRA;
        uint32_t *pixels = new uint32_t[image->width * image->height];

        for (int i = 0, y = 0; y < image->height; ++y)
        {
            for (int x = 0; x < image->width; ++x, ++i)
            {
                pixels[i] = image->f.get_pixel(image, x, y) | 0xff000000;;
            }
        }

        unsigned bpp = 4;
        unsigned scanline = png.width * bpp;
        bool success = !!png_image_write_to_stdio(&png, file, 0, pixels, scanline, NULL);
        delete[] pixels;
        fclose(file);

        if (!success)
        {
            cerr << "error: png_image_write_to_stdio(): failed to write PNG to " << path << endl;
        }
    }
};

class XGameControls : public GameControls
{
    Display *display;
    Window root;

public:

    XGameControls(Display *display, Window root) :
        display(display),
        root(root)
    {
    }

    void fire()
    {
        auto keycode = XKeysymToKeycode(display, XK_space);
        XTestFakeKeyEvent(display, keycode, True, 0);
        //usleep(500);
        XTestFakeKeyEvent(display, keycode, False, 0);
        XFlush(display);
    }

    void focus(int x, int y)
    {
        XWarpPointer(display, None, root, 0, 0, 0, 0, x, y);

        Window window;
        Window child;
        int dummy;
        unsigned udummy;
        XQueryPointer(display, root, &window, &child, &dummy, &dummy, &dummy, &dummy, &udummy);

        if (window)
        {
            XSetInputFocus(display, child, RevertToNone, CurrentTime);
        }
    }

    void move(int x, int y)
    {
        XWarpPointer(display, None, root, 0, 0, 0, 0, x, y);
    }

    void click(int x, int y)
    {
        clickButton(x, y, Button1);
    }

    void clickButton(int x, int y, int button)
    {
        XWarpPointer(display, None, root, 0, 0, 0, 0, x, y);

        XEvent event;
        memset(&event, 0, sizeof(event));

        event.type = ButtonPress;
        event.xbutton.button = button;
        event.xbutton.same_screen = True;

        XQueryPointer(display, root, &event.xbutton.root, &event.xbutton.window, &event.xbutton.x_root, &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y, &event.xbutton.state);

        event.xbutton.subwindow = event.xbutton.window;
        while(event.xbutton.subwindow)
        {
            event.xbutton.window = event.xbutton.subwindow;

            XQueryPointer(display, event.xbutton.window, &event.xbutton.root, &event.xbutton.subwindow, &event.xbutton.x_root, &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y, &event.xbutton.state);
        }

        if (XSendEvent(display, PointerWindow, True, 0xfff, &event) == 0)
        {
            cerr << "error: failed to simulate click (mousedown)" << endl;
        }
        XFlush(display);

        //usleep(100);
        event.type = ButtonRelease;
        event.xbutton.state = 0x100;
        if (XSendEvent(display, PointerWindow, True, 0xfff, &event) == 0)
        {
            cerr << "error: failed to simulate click (mouseup)" << endl;
        }

        XFlush(display);
    }
};


int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    auto display = XOpenDisplay((char *) NULL);
    if (display != nullptr)
    {
        auto screen = XDefaultScreen(display);
        auto root = XRootWindow(display, screen);

        XWindowAttributes root_attr;
        XGetWindowAttributes(display, root, &root_attr);
        auto width = root_attr.width;
        auto height = root_attr.height;

        cerr << "screen: width=" << width << ", height=" << height << endl;

        XGameControls controls(display, root);
        XImageFrame frame(display, root);

        Game game(controls, width, height, 1);
        while (game.step(frame))
        {
            this_thread::sleep_for(chrono::milliseconds(1));
        }
    }
    else
    {
        cerr << "error: failed to open display" << endl;
    }

    XCloseDisplay(display);
    return 0;
}

