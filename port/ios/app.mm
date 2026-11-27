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
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

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
// of the controls' part of the view (x from the right edge when negative, from the middle when
// centred; y from the bottom when negative), in points.
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
constexpr float kCreaseMargin = 70.0f;   // below the hinge, where the folded screen is flat again

}  // namespace

// The picture: the layer the Metal renderer presents to.
@interface PictureView : UIView
@end
@implementation PictureView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

// The screen: the picture, and the controls over it. With the phone folded half way and standing
// on one half, the app turns upright (it is otherwise held on its side: the system would show
// its picture small across the hinge), the hinge is across the view's middle, the picture is the
// upper half's and the controls are on the lower half.
@interface GameView : UIView
@property (nonatomic, readonly) PictureView* picture;
@property (nonatomic, readonly) bool halfFolded;
@property (nonatomic, readonly) bool flatOpen;   // the hinge fully open: the phone may be held upright too
@end

@implementation GameView {
    NSMutableDictionary<NSValue*, NSArray<NSNumber*>*>* _touches;   // each touch: its role, and what it started with
    NSMutableArray<CALayer*>* _drawn;
    bool _halfFolded;
    NSInteger _hingeStatus;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _touches = [NSMutableDictionary new];
        _drawn = [NSMutableArray new];
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.blackColor;
        _picture = [[PictureView alloc] initWithFrame:self.bounds];
        _picture.userInteractionEnabled = NO;
        _picture.layer.contentsGravity = kCAGravityResize;   // (the frame is made of the picture's own shape)
        _picture.layer.backgroundColor = UIColor.blackColor.CGColor;
        [self addSubview:_picture];
        _hingeStatus = -1;
        if (@available(iOS 27.1, *)) {
            __weak GameView* weak = self;
            [self addInteraction:[[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction*, UIHingeInteractionUpdate* update) {
                [weak hinge:update.hinge];
            }]];
        }
    }
    return self;
}

// The hinge moved. Half way is partially open and well short of flat (held nearly flat in the
// hands the phone is still one screen).
- (void)hinge:(UIHinge*)hinge API_AVAILABLE(ios(27.1))
{
    const NSInteger status = hinge ? hinge.status : -1;
    const double degrees = hinge ? hinge.angle * 180.0 / M_PI : 0.0;
    const bool half = hinge && hinge.status == UIHingeStatusPartiallyOpen && degrees < (_halfFolded ? 155.0 : 145.0);
    if (status != _hingeStatus || half != _halfFolded)
        std::fprintf(stderr, "ios: the hinge: status %ld at %.0f degrees: %s\n", (long)status, degrees, half ? "the picture on the upper half" : "one screen");
    _hingeStatus = status;
    const bool flat = hinge && !half && hinge.status != UIHingeStatusClosed && hinge.status != UIHingeStatusUnknown;
    if (half != _halfFolded || flat != _flatOpen) {
        _halfFolded = half;
        _flatOpen = flat;
        [self.window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
        [self setNeedsLayout];
    }
}

// Upright and folded half way, the view is two halves: the picture's above the controls'. (Upright
// and flat, the picture is the whole view and the controls are over its lower part.)
- (bool)split { return _halfFolded && self.bounds.size.height > self.bounds.size.width; }

// The part of the view the controls are on.
- (CGRect)controls
{
    // (no margin at the sides: the buttons keep their places from the view's edges on every
    // screen, over a camera's cutout if one is there, and not pushed off centre by it)
    UIEdgeInsets safe = self.safeAreaInsets;
    safe.left = safe.right = 0;
    CGRect all = UIEdgeInsetsInsetRect(self.bounds, safe);
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {   // a tablet: the lower half, where the hands hold it
        const CGFloat top = CGRectGetMidY(self.bounds);
        all = CGRectMake(all.origin.x, top, all.size.width, CGRectGetMaxY(all) - top);
    } else if (self.bounds.size.height > self.bounds.size.width) {   // upright: the lower half (in reach), clear of the fold's curve
        const CGFloat top = CGRectGetMidY(self.bounds) + kCreaseMargin;
        all = CGRectMake(all.origin.x, top, all.size.width, CGRectGetMaxY(all) - top);
    }
    return all;
}

- (CGPoint)centreOf:(const ScreenButton&)b
{
    const CGRect r = [self controls];
    if (b.centred && UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        // (a tablet is held by its sides: the middle is out of the thumbs' reach, so SELECT is
        // under the left shoulder buttons and START under the right ones)
        return CGPointMake(b.x < 0 ? CGRectGetMinX(r) + 70 : CGRectGetMaxX(r) - 70, CGRectGetMinY(r) + 114);
    }
    const CGFloat x = b.centred ? CGRectGetMidX(r) + b.x : b.x < 0 ? CGRectGetMaxX(r) + b.x : CGRectGetMinX(r) + b.x;
    const CGFloat y = b.y < 0 ? CGRectGetMaxY(r) + b.y : CGRectGetMinY(r) + b.y;
    return CGPointMake(x, y);
}

// The picture's place, and the buttons drawn over the view (not while a controller is connected).
- (void)layoutSubviews
{
    [super layoutSubviews];
    {   // The picture fills the view, or its upper half: the game's view takes its shape, and the
        // layer's image is of the screen's own pixels (docs/IOS.md, "The picture's shape").
        CGRect place = self.bounds;
        if ([self split])
            place.size.height = std::floor(place.size.height / 2);
        _picture.frame = place;
        const CGFloat scale = self.window.screen ? self.window.screen.nativeScale : g_screen.nativeScale;
        const CGSize pixels = CGSizeMake(std::round(place.size.width * scale), std::round(place.size.height * scale));
        if (pixels.width >= 64 && pixels.height >= 64) {
            CAMetalLayer* layer = (CAMetalLayer*)_picture.layer;
            layer.contentsScale = scale;
            if (!CGSizeEqualToSize(layer.drawableSize, pixels)) {
                layer.drawableSize = pixels;
                port::gpu::SetScreenShape((unsigned)pixels.width, (unsigned)pixels.height);
                std::fprintf(stderr, "ios: the picture at %.0f by %.0f pixels\n", pixels.width, pixels.height);
            }
        }
    }
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
        t.zPosition = 1;   // (over the picture and the message)
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
            const int role = p.x < self.bounds.size.width * 0.5 ? 1 : 2;
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

@interface GameController : UIViewController <UIDocumentPickerDelegate>
@end

@implementation GameController {
    UILabel* _message;
    UIButton* _choose;
    NSString* _note;   // what became of the file last chosen, when it was not a disc image to play
    NSTimer* _look;
    bool _started;
}
- (void)loadView { self.view = [[GameView alloc] initWithFrame:g_screen.bounds]; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    // On its side; upright when folded half way; either way when open flat, where an upright
    // view is laid out as the half-folded one is (the picture above, the controls below).
    GameView* view = (GameView*)self.view;
    return view.halfFolded ? UIInterfaceOrientationMaskPortrait
           : view.flatOpen ? UIInterfaceOrientationMaskLandscape | UIInterfaceOrientationMaskPortrait : UIInterfaceOrientationMaskLandscape;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    const CGRect all = self.view.bounds;
    _message = [[UILabel alloc] initWithFrame:CGRectMake(40, 20, all.size.width - 80, all.size.height - 110)];
    _message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _message.adjustsFontSizeToFitWidth = YES;
    _message.minimumScaleFactor = 0.6;
    _message.numberOfLines = 0;
    _message.textColor = UIColor.whiteColor;
    _message.font = [UIFont systemFontOfSize:17];
    _message.textAlignment = NSTextAlignmentCenter;
    _message.hidden = YES;
    [self.view addSubview:_message];
    // Until there is a disc image, a button to choose one anywhere the Files app reaches (iCloud
    // Drive, On My iPhone, a drive): it is copied into the app's folder, and not asked for again.
    UIButtonConfiguration* look = [UIButtonConfiguration filledButtonConfiguration];
    look.title = @"Choose a disc image…";
    _choose = [UIButton buttonWithConfiguration:look primaryAction:[UIAction actionWithHandler:^(UIAction*) { [self choose]; }]];
    _choose.frame = CGRectMake(all.size.width / 2 - 120, all.size.height - 80, 240, 48);
    _choose.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin | UIViewAutoresizingFlexibleTopMargin;
    _choose.hidden = YES;
    [self.view addSubview:_choose];
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
        [text appendString:@"Choose your own disc image of \"Sly Cooper and the Thievius Raccoonus\" (North American version, SCUS-97198) "
                            "with the button below, or copy it into this app's folder: in the Files app under On My iPhone, or in the "
                            "Finder with the phone connected. Any file name will do. The game starts when it is there."];
        for (const port::DiscCandidate& c : seen)
            [text appendFormat:@"\n\n%s: %s", c.name.c_str(), port::DiscKindReason(c.kind)];
        if (_note)
            [text appendFormat:@"\n\n%@", _note];
        _message.text = text;
        _message.hidden = NO;
        _choose.hidden = self.presentedViewController != nil;
        for (CALayer* l in self.view.layer.sublayers)   // (the pad's buttons are the game's)
            if ([l isKindOfClass:[CATextLayer class]])
                l.hidden = YES;
        return;
    }
    _started = true;
    _message.hidden = YES;
    _choose.hidden = YES;
    [(GameView*)self.view showControls];
    [_look invalidate];
    _look = nil;
    // The picture's shape is its view's (docs/IOS.md), and follows it from here on.
    [self.view layoutIfNeeded];
    port::gpu::SetScreenLayer((__bridge void*)((GameView*)self.view).picture.layer);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    pthread_create(&thread, &attr, GameThread, nullptr);
    pthread_attr_destroy(&attr);
}

// A disc image chosen in the system's file browser.
- (void)choose
{
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    _choose.hidden = YES;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
    (void)controller;
    _choose.hidden = _started;
}

// The system's copy of the chosen file is moved into the app's folder, where the disc image is
// looked for. A file that is not a disc image to play is not kept: the reason is shown.
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    (void)controller;
    NSURL* from = urls.firstObject;
    if (!from || _started)
        return;
    NSFileManager* files = NSFileManager.defaultManager;
    NSString* name = from.lastPathComponent;
    NSString* to = [[NSString stringWithUTF8String:g_documents.c_str()] stringByAppendingPathComponent:name];
    NSError* error = nil;
    _note = nil;
    if ([files fileExistsAtPath:to]) {   // (a file of that name is there already: it is the one looked at)
        [files removeItemAtURL:from error:nil];
    } else if (![files moveItemAtURL:from toURL:[NSURL fileURLWithPath:to] error:&error]) {
        _note = [NSString stringWithFormat:@"%@ could not be copied here: %@", name, error.localizedDescription];
        std::fprintf(stderr, "ios: the chosen file was not moved: %s\n", error.localizedDescription.UTF8String);
        [files removeItemAtURL:from error:nil];
    } else {
        std::vector<port::DiscCandidate> seen;
        if (port::FindDisc(g_documents, &seen).empty()) {
            const char* reason = "it is not a disc image";
            for (const port::DiscCandidate& c : seen)
                if (c.name == name.UTF8String)
                    reason = port::DiscKindReason(c.kind);
            _note = [NSString stringWithFormat:@"%@ was not kept: %s", name, reason];
            [files removeItemAtPath:to error:nil];
        }
    }
    [self look];
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
