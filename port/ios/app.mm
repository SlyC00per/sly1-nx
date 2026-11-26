// SPDX-License-Identifier: AGPL-3.0-or-later
// The iPhone's app (docs/IOS.md): a UIKit application whose one view is the screen the Metal
// renderer presents to, with the game (port/host/main.cpp's sly1_main) on a thread of its own.
//
// The player's disc image is looked for in the app's Documents folder, which the Files app and
// the Finder show; the saves are kept there too. A controller is the pad (port/apple/pad_gc.mm);
// without one, the screen is: the left side a stick, the right side the camera, and buttons drawn
// over the picture.
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "port/apple.h"
#include "port/gpu.h"
#include "port/iso.h"
#include "port/retail_data.h"
#include "port/system.h"

int sly1_main(int argc, char** argv);

namespace {

std::string g_documents, g_disc;
UIScreen* g_screen;   // the scene's
std::atomic<std::uint64_t> g_touchPad{0x808080800000ull};   // what the screen's controls hold, as port::g_hostPad

void* GameThread(void*)
{
    static std::string save = g_documents + "/save";
    static char a0[] = "sly1", a1[] = "--iso", a3[] = "--save", a5[] = "--seconds", a6[] = "0";
    char* argv[] = {a0, a1, g_disc.data(), a3, save.data(), a5, a6, nullptr};
    sly1_main(7, argv);
    return nullptr;
}

// A button of the screen's: its bit of the pad, its letter, and where its centre is from a corner
// of the view (x from the right edge when negative, from the middle when centred; y from the
// bottom when negative), in points.
struct ScreenButton {
    std::uint16_t bit;
    const char* label;
    float x, y;
    bool centred;
};
const ScreenButton kButtons[] = {
    {0x0040, "✕", -110, -65, false},    // Cross
    {0x0020, "○", -55, -120, false},    // Circle
    {0x0080, "□", -165, -120, false},   // Square
    {0x0010, "△", -110, -175, false},   // Triangle
    {0x0004, "L1", 70, 44, false},
    {0x0001, "L2", 160, 44, false},
    {0x0008, "R1", -70, 44, false},
    {0x0002, "R2", -160, 44, false},
    {0x0800, "START", 55, 30, true},
    {0x0100, "SELECT", -55, 30, true},
};
constexpr float kButtonRadius = 30.0f, kStickRadius = 60.0f;

}  // namespace

@interface GameView : UIView
@end

@implementation GameView {
    NSMutableDictionary<NSValue*, NSArray<NSNumber*>*>* _touches;   // each touch: its role, and what it started with
    NSMutableArray<CALayer*>* _drawn;
}
+ (Class)layerClass { return [CAMetalLayer class]; }

- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _touches = [NSMutableDictionary new];
        _drawn = [NSMutableArray new];
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.blackColor;
        self.layer.contentsGravity = kCAGravityResizeAspect;
        self.layer.backgroundColor = UIColor.blackColor.CGColor;
    }
    return self;
}

- (CGPoint)centreOf:(const ScreenButton&)b
{
    const CGSize size = self.bounds.size;
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat x = b.centred ? size.width / 2 + b.x : b.x < 0 ? size.width - safe.right + b.x : safe.left + b.x;
    const CGFloat y = b.y < 0 ? size.height - safe.bottom + b.y : safe.top + b.y;
    return CGPointMake(x, y);
}

// The buttons drawn over the picture (not while a controller is connected).
- (void)layoutSubviews
{
    [super layoutSubviews];
    for (CALayer* l in _drawn)
        [l removeFromSuperlayer];
    [_drawn removeAllObjects];
    for (const ScreenButton& b : kButtons) {
        const CGPoint c = [self centreOf:b];
        CATextLayer* t = [CATextLayer layer];
        t.string = [NSString stringWithUTF8String:b.label];
        t.fontSize = std::strlen(b.label) > 3 ? 11 : 18;
        t.alignmentMode = kCAAlignmentCenter;
        t.contentsScale = g_screen.scale;
        t.foregroundColor = [UIColor colorWithWhite:1 alpha:0.55].CGColor;
        t.backgroundColor = [UIColor colorWithWhite:0 alpha:0.18].CGColor;
        t.borderColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
        t.borderWidth = 1.5;
        t.cornerRadius = kButtonRadius;
        t.frame = CGRectMake(c.x - kButtonRadius, c.y - kButtonRadius, kButtonRadius * 2, kButtonRadius * 2);
        // (the letter in the circle's middle: a text layer draws from its top)
        t.bounds = CGRectMake(0, -(kButtonRadius - t.fontSize * 0.62), kButtonRadius * 2, kButtonRadius * 2);
        [self.layer addSublayer:t];
        [_drawn addObject:t];
    }
    [self showControls];
}

- (void)showControls
{
    bool controller = false;
    for (GCController* c in GCController.controllers)
        controller = controller || c.extendedGamepad != nil;
    for (CALayer* l in _drawn)
        l.hidden = controller;
}

// What the touches hold, as a pad: role 0 a button (its bit), 1 the left stick, 2 the camera's.
- (void)publish
{
    std::uint64_t buttons = 0;
    float stick[2][2] = {{0, 0}, {0, 0}};
    for (NSValue* key in _touches) {
        NSArray<NSNumber*>* t = _touches[key];
        const int role = t[0].intValue;
        if (role == 0) {
            buttons |= (std::uint64_t)t[1].unsignedIntValue;
        } else {
            stick[role - 1][0] = std::clamp((t[3].floatValue - t[1].floatValue) / kStickRadius, -1.0f, 1.0f);
            stick[role - 1][1] = std::clamp((t[4].floatValue - t[2].floatValue) / kStickRadius, -1.0f, 1.0f);
        }
    }
    auto byte = [](float v) { return (std::uint64_t)std::clamp(std::lround(128.0f + v * 127.0f), 0l, 255l); };
    g_touchPad.store(buttons | byte(stick[1][0]) << 16 | byte(stick[1][1]) << 24 | byte(stick[0][0]) << 32 | byte(stick[0][1]) << 40, std::memory_order_relaxed);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    (void)event;
    for (UITouch* touch in touches) {
        const CGPoint p = [touch locationInView:self];
        NSValue* key = [NSValue valueWithNonretainedObject:touch];
        NSArray<NSNumber*>* made = nil;
        for (const ScreenButton& b : kButtons) {
            const CGPoint c = [self centreOf:b];
            if (std::hypot(p.x - c.x, p.y - c.y) <= kButtonRadius * 1.35f) {
                made = @[@0, @(b.bit)];
                break;
            }
        }
        if (!made) {   // a stick where the touch began: the left side the player's, the right the camera's
            const int role = p.x < self.bounds.size.width * 0.45 ? 1 : 2;
            bool taken = false;
            for (NSValue* k in _touches)
                taken = taken || _touches[k][0].intValue == role;
            if (taken)
                continue;
            made = @[@(role), @(p.x), @(p.y), @(p.x), @(p.y)];
        }
        _touches[key] = made;
    }
    [self publish];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    (void)event;
    for (UITouch* touch in touches) {
        NSValue* key = [NSValue valueWithNonretainedObject:touch];
        NSArray<NSNumber*>* t = _touches[key];
        if (!t || t[0].intValue == 0)
            continue;
        const CGPoint p = [touch locationInView:self];
        _touches[key] = @[t[0], t[1], t[2], @(p.x), @(p.y)];
    }
    [self publish];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    (void)event;
    for (UITouch* touch in touches)
        [_touches removeObjectForKey:[NSValue valueWithNonretainedObject:touch]];
    [self publish];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self touchesEnded:touches withEvent:event];
}
@end

@interface GameController : UIViewController
@end

@implementation GameController {
    UILabel* _message;
    NSTimer* _look;
    bool _started;
}
- (void)loadView { self.view = [[GameView alloc] initWithFrame:g_screen.bounds]; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)viewDidLoad
{
    [super viewDidLoad];
    _message = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 40, 20)];
    _message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _message.numberOfLines = 0;
    _message.textColor = UIColor.whiteColor;
    _message.font = [UIFont systemFontOfSize:17];
    _message.textAlignment = NSTextAlignmentCenter;
    _message.hidden = YES;
    [self.view addSubview:_message];
    // the pad, 120 times a second: a controller's state, or the screen's controls
    port::apple::WatchControllers();
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0 repeats:YES block:^(NSTimer*) {
        std::uint64_t pad;
        if (!port::apple::ControllerPad(&pad))
            pad = g_touchPad.load(std::memory_order_relaxed);
        port::g_hostPad.store(pad, std::memory_order_relaxed);
    }];
    NSNotificationCenter* centre = NSNotificationCenter.defaultCenter;
    for (NSNotificationName name in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
        [centre addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) { [(GameView*)self.view showControls]; }];
    [self look];
    if (!_started)
        _look = [NSTimer scheduledTimerWithTimeInterval:2.0 repeats:YES block:^(NSTimer*) { [self look]; }];
}

// The player's disc image looked for, and the game started when there is one.
- (void)look
{
    if (_started)
        return;
    std::vector<port::DiscCandidate> seen;
    g_disc = port::FindDisc(g_documents, &seen);
    if (g_disc.empty()) {
        NSMutableString* text = [NSMutableString stringWithString:@"Sly Cooper\n\n"];
        [text appendString:seen.empty() ? @"No disc image was found.\n\n" : @"No disc image that can be played was found.\n\n"];
        [text appendString:@"Copy your own disc image of \"Sly Cooper and the Thievius Raccoonus\" (North American version, SCUS-97198) "
                            "into this app's folder: in the Files app under On My iPhone, or in the Finder with the phone connected. "
                            "Any file name will do. The game starts when it is there."];
        for (const port::DiscCandidate& c : seen)
            [text appendFormat:@"\n\n%s: %s", c.name.c_str(), port::DiscKindReason(c.kind)];
        _message.text = text;
        _message.hidden = NO;
        return;
    }
    _started = true;
    _message.hidden = YES;
    [_look invalidate];
    _look = nil;
    // The picture's shape is the screen's (docs/IOS.md): wider than it is tall, whichever way it is held.
    const CGSize native = g_screen.nativeBounds.size;
    port::SetAspect((float)(std::max(native.width, native.height) / std::min(native.width, native.height)));
    port::gpu::SetScreenLayer((__bridge void*)self.view.layer);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    pthread_create(&thread, &attr, GameThread, nullptr);
    pthread_attr_destroy(&attr);
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options
{
    (void)options;
    g_documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject.UTF8String;
    mkdir(g_documents.c_str(), 0755);
    mkdir((g_documents + "/save").c_str(), 0755);
    {   // What the program prints, kept for looking at afterwards: this run's and the one before's.
        const std::string log = g_documents + "/sly1-log.txt", before = g_documents + "/sly1-log-before.txt";
        std::rename(log.c_str(), before.c_str());
        if (std::freopen(log.c_str(), "w", stderr))
            std::setvbuf(stderr, nullptr, _IOLBF, 0);
    }
    application.idleTimerDisabled = YES;   // (a controller's buttons are not touches: the screen would dim)
    return YES;
}
@end

// The app's one scene (the lifecycle the system asks of an app now): its window, and the game
// held while the scene is not the active one.
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow* window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options
{
    (void)session;
    (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]])
        return;
    UIWindowScene* windowScene = (UIWindowScene*)scene;
    g_screen = windowScene.screen;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    self.window.rootViewController = [GameController new];
    [self.window makeKeyAndVisible];
}
// Off the screen nothing may be drawn: the game waits at its frame's end, and its sound with it.
- (void)sceneWillResignActive:(UIScene*)scene
{
    (void)scene;
    port::gpu::Suspend(true);
}
- (void)sceneDidBecomeActive:(UIScene*)scene
{
    (void)scene;
    port::gpu::Suspend(false);
}
@end

int main(int argc, char** argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
