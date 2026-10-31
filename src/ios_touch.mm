// On-screen controller for iOS: a UIKit layer over the game view. It feeds the same
// virtual pad as the keyboard (host.cpp), and hides itself while a real controller is
// connected; touching the screen brings it back.
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include "ios_touch.h"

namespace {
std::atomic<uint32_t> g_buttons{0};
std::atomic<int32_t> g_lx{0}, g_ly{0};
std::atomic<int> g_menu{0};

enum Control { kNone = -1, kStick = -2 };

struct Button {
    uint32_t bit;
    NSString* label;
    CAShapeLayer* layer;
    CATextLayer* text;
    CGPoint centre;
    CGFloat radius;
};
}  // namespace

@interface FPTouchView : UIView
@end

static bool g_forced_visible;

@implementation FPTouchView {
    bool _collapsed;
    Button _buttons[13];
    int _count;
    CAShapeLayer* _base;
    CAShapeLayer* _knob;
    UITouch* _stickTouch;
    CGPoint _stickOrigin;
    NSMutableDictionary<NSValue*, NSNumber*>* _held;   // touch -> button index
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _held = [NSMutableDictionary dictionary];
    const struct { uint32_t bit; NSString* label; } defs[] = {
        {FP_TOUCH_CROSS, @"✕"}, {FP_TOUCH_CIRCLE, @"○"}, {FP_TOUCH_SQUARE, @"□"}, {FP_TOUCH_TRIANGLE, @"△"},
        {FP_TOUCH_L1, @"L1"}, {FP_TOUCH_R1, @"R1"}, {FP_TOUCH_L2, @"L2"}, {FP_TOUCH_R2, @"R2"},
        {FP_TOUCH_SELECT, @"SELECT"}, {FP_TOUCH_START, @"START"}, {FP_TOUCH_L3, @"L3"}, {FP_TOUCH_R3, @"R3"},
        {FP_TOUCH_MENU, @"☰"},
    };
    _count = 13;
    for (int i = 0; i < _count; i++) {
        _buttons[i].bit = defs[i].bit;
        _buttons[i].label = defs[i].label;
        _buttons[i].layer = [self circleLayer];
        CATextLayer* t = [CATextLayer layer];
        t.string = defs[i].label;
        t.alignmentMode = kCAAlignmentCenter;
        t.foregroundColor = [UIColor colorWithWhite:1 alpha:0.85].CGColor;
        t.contentsScale = UIScreen.mainScreen.scale;
        [self.layer addSublayer:_buttons[i].layer];
        [self.layer addSublayer:t];
        _buttons[i].text = t;
    }
    _base = [self circleLayer];
    _knob = [self circleLayer];
    _knob.fillColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
    _base.hidden = _knob.hidden = YES;
    [self.layer addSublayer:_base];
    [self.layer addSublayer:_knob];
    return self;
}

- (CAShapeLayer*)circleLayer {
    CAShapeLayer* l = [CAShapeLayer layer];
    l.fillColor = [UIColor colorWithWhite:0 alpha:0.25].CGColor;
    l.strokeColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
    l.lineWidth = 2;
    return l;
}

- (void)place:(int)i at:(CGPoint)c radius:(CGFloat)r {
    _buttons[i].centre = c;
    _buttons[i].radius = r;
    _buttons[i].layer.path = [UIBezierPath bezierPathWithArcCenter:c radius:r startAngle:0 endAngle:2 * M_PI clockwise:YES].CGPath;
    const CGFloat size = _buttons[i].label.length > 2 ? r * 0.45 : r * 0.8;
    _buttons[i].text.fontSize = size;
    _buttons[i].text.frame = CGRectMake(c.x - r, c.y - size * 0.62, 2 * r, size * 1.3);
}

// Layout unit in points: about a thumb's width on a phone, capped so a tablet does not get giant buttons.
- (CGFloat)unit {
    const CGFloat m = MIN(self.bounds.size.width, self.bounds.size.height);
    return MAX(24.0, MIN(40.0, m / 15.0));
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGRect b = UIEdgeInsetsInsetRect(self.bounds, safe);
    const CGFloat u = [self unit];
    const CGFloat r = u * 0.9, maxX = CGRectGetMaxX(b), maxY = CGRectGetMaxY(b), minX = b.origin.x, minY = b.origin.y;
    const CGPoint face = CGPointMake(maxX - 3.4 * u, maxY - 3.4 * u);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [self place:0 at:CGPointMake(face.x, face.y + 2 * u) radius:r];       // cross
    [self place:1 at:CGPointMake(face.x + 2 * u, face.y) radius:r];       // circle
    [self place:2 at:CGPointMake(face.x - 2 * u, face.y) radius:r];       // square
    [self place:3 at:CGPointMake(face.x, face.y - 2 * u) radius:r];       // triangle
    [self place:4 at:CGPointMake(minX + 1.4 * u, minY + 3.4 * u) radius:r * 0.8];   // L1
    [self place:5 at:CGPointMake(maxX - 1.4 * u, minY + 3.4 * u) radius:r * 0.8];   // R1
    [self place:6 at:CGPointMake(minX + 1.4 * u, minY + 1.4 * u) radius:r * 0.8];   // L2
    [self place:7 at:CGPointMake(maxX - 1.4 * u, minY + 1.4 * u) radius:r * 0.8];   // R2
    [self place:8 at:CGPointMake(CGRectGetMidX(b) - 1.6 * u, minY + 1.0 * u) radius:r * 0.7];   // select
    [self place:9 at:CGPointMake(CGRectGetMidX(b) + 1.6 * u, minY + 1.0 * u) radius:r * 0.7];   // start
    [self place:10 at:CGPointMake(minX + 6.0 * u, maxY - 1.0 * u) radius:r * 0.6];  // L3
    [self place:11 at:CGPointMake(maxX - 6.6 * u, maxY - 1.0 * u) radius:r * 0.6]; // R3
    [self place:12 at:CGPointMake(CGRectGetMidX(b), minY + 1.0 * u) radius:r * 0.6];      // settings menu
    [CATransaction commit];
}

- (CGFloat)stickRange { return [self unit] * 1.6; }

- (int)buttonAt:(CGPoint)p {
    int best = kNone;
    CGFloat bestD = 0;
    for (int i = 0; i < _count; i++) {
        const CGFloat d = hypot(p.x - _buttons[i].centre.x, p.y - _buttons[i].centre.y);
        if (d <= _buttons[i].radius * 1.25 && (best == kNone || d < bestD)) { best = i; bestD = d; }
    }
    return best;
}

- (void)showStickAt:(CGPoint)o knob:(CGPoint)k {
    const CGFloat range = [self stickRange];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _base.path = [UIBezierPath bezierPathWithArcCenter:o radius:range startAngle:0 endAngle:2 * M_PI clockwise:YES].CGPath;
    _knob.path = [UIBezierPath bezierPathWithArcCenter:k radius:range * 0.45 startAngle:0 endAngle:2 * M_PI clockwise:YES].CGPath;
    _base.hidden = _knob.hidden = NO;
    [CATransaction commit];
}

- (void)moveStick:(CGPoint)p {
    const CGFloat range = [self stickRange];
    CGFloat dx = p.x - _stickOrigin.x, dy = p.y - _stickOrigin.y;
    const CGFloat len = hypot(dx, dy);
    if (len > range) { dx *= range / len; dy *= range / len; }
    g_lx = (int32_t)lround(dx / range * 32767);
    g_ly = (int32_t)lround(dy / range * 32767);
    [self showStickAt:_stickOrigin knob:CGPointMake(_stickOrigin.x + dx, _stickOrigin.y + dy)];
}

- (void)refreshButtons {
    uint32_t mask = 0;
    for (NSNumber* n in _held.allValues) mask |= _buttons[n.intValue].bit;
    if ((mask & FP_TOUCH_MENU) && !(g_buttons.load() & FP_TOUCH_MENU)) g_menu = 1;
    g_buttons = mask;
    for (int i = 0; i < _count; i++)
        _buttons[i].layer.fillColor = [UIColor colorWithWhite:(mask & _buttons[i].bit) ? 1 : 0
                                                        alpha:(mask & _buttons[i].bit) ? 0.45 : 0.25].CGColor;
}

// Collapsed (a controller is connected) the view stays in place, drawn as nothing, so a
// touch can still bring the controls back.
- (void)setCollapsed:(bool)collapsed {
    if (_collapsed == collapsed) return;
    _collapsed = collapsed;
    for (CALayer* l in self.layer.sublayers) l.opacity = collapsed ? 0 : 1;
    if (collapsed) { [_held removeAllObjects]; _stickTouch = nil; g_buttons = 0; g_lx = 0; g_ly = 0; }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_collapsed) { g_forced_visible = true; return; }
    for (UITouch* t in touches) {
        const CGPoint p = [t locationInView:self];
        const int b = [self buttonAt:p];
        if (b != kNone) {
            _held[[NSValue valueWithNonretainedObject:t]] = @(b);
        } else if (!_stickTouch && p.x < self.bounds.size.width * 0.5) {
            _stickTouch = t;
            _stickOrigin = p;
            [self moveStick:p];
        }
    }
    [self refreshButtons];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_collapsed) return;
    for (UITouch* t in touches) {
        if (t == _stickTouch) { [self moveStick:[t locationInView:self]]; continue; }
        NSValue* key = [NSValue valueWithNonretainedObject:t];
        if (!_held[key]) continue;
        const int b = [self buttonAt:[t locationInView:self]];   // sliding across the face buttons
        if (b != kNone) _held[key] = @(b);
    }
    [self refreshButtons];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* t in touches) {
        if (t == _stickTouch) {
            _stickTouch = nil;
            g_lx = 0; g_ly = 0;
            _base.hidden = _knob.hidden = YES;
        }
        [_held removeObjectForKey:[NSValue valueWithNonretainedObject:t]];
    }
    [self refreshButtons];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}
@end

static FPTouchView* g_view;

extern "C" void fp_touch_tick(void) {
    if (!g_view) {
        UIWindow* window = nil;
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
            if ([scene isKindOfClass:UIWindowScene.class])
                for (UIWindow* w in ((UIWindowScene*)scene).windows)
                    if (w.isKeyWindow || !window) window = w;
        UIView* host = window.rootViewController.view;
        if (!host) return;
        g_view = [[FPTouchView alloc] initWithFrame:host.bounds];
        [host addSubview:g_view];
        fprintf(stderr, "[fp-touch] on-screen controller attached\n");
    }
    // A connected controller hides the touch layer until the screen is touched again.
    const bool pad = GCController.controllers.count > 0;
    static bool had_pad;
    if (pad && !had_pad) g_forced_visible = false;
    had_pad = pad;
    [g_view setCollapsed:pad && !g_forced_visible];
    [g_view.superview bringSubviewToFront:g_view];
}

extern "C" int fp_touch_take_menu(void) { return g_menu.exchange(0); }
extern "C" int fp_touch_keyboard_connected(void) { return GCKeyboard.coalescedKeyboard != nil; }
extern "C" int fp_touch_controls_visible(void) { return g_view && (!GCController.controllers.count || g_forced_visible); }
extern "C" int fp_touch_controller_family(void) {
    NSString* category = GCController.controllers.firstObject.productCategory.lowercaseString;
    if ([category containsString:@"xbox"]) return 2;
    if ([category containsString:@"nintendo"] || [category containsString:@"switch"]) return 3;
    if ([category containsString:@"dual"] || [category containsString:@"playstation"]) return 1;
    return 0;
}

extern "C" void fp_touch_overlay_area(float area[6]) {
    const CGRect bounds = g_view.bounds;
    const CGFloat w = bounds.size.width, h = bounds.size.height;
    if (w <= 0 || h <= 0) {
        const float fallback[6] = {0.06f, 0.04f, 0.94f, 0.92f, 0, 0};
        for (int i = 0; i < 6; ++i) area[i] = fallback[i];
        return;
    }
    const UIEdgeInsets safe = g_view.safeAreaInsets;
    // Extra space keeps the panel border away from curved corners and gestures.
    area[0] = (safe.left + 8) / w;
    // Keep status panels below SELECT / menu / START touch controls.
    area[1] = (MAX(safe.top, 48) + 8) / h;
    area[2] = 1 - (safe.right + 8) / w;
    area[3] = 1 - (safe.bottom + 8) / h;
    area[4] = w;
    area[5] = h;
}

extern "C" unsigned fp_touch_pad_bits(void) {
    // PS3 pad bits (cellPad.h): the stick doubles as the d-pad for menus.
    const uint32_t b = g_buttons.load();
    const int x = g_lx.load(), y = g_ly.load(), dead = 16000;
    unsigned out = 0;
    if (b & FP_TOUCH_SELECT) out |= 1u << 0;
    if (b & FP_TOUCH_L3) out |= 1u << 1;
    if (b & FP_TOUCH_R3) out |= 1u << 2;
    if (b & FP_TOUCH_START) out |= 1u << 3;
    if (y < -dead) out |= 1u << 4;
    if (x > dead) out |= 1u << 5;
    if (y > dead) out |= 1u << 6;
    if (x < -dead) out |= 1u << 7;
    if (b & FP_TOUCH_L2) out |= 1u << 8;
    if (b & FP_TOUCH_R2) out |= 1u << 9;
    if (b & FP_TOUCH_L1) out |= 1u << 10;
    if (b & FP_TOUCH_R1) out |= 1u << 11;
    if (b & FP_TOUCH_TRIANGLE) out |= 1u << 12;
    if (b & FP_TOUCH_CIRCLE) out |= 1u << 13;
    if (b & FP_TOUCH_CROSS) out |= 1u << 14;
    if (b & FP_TOUCH_SQUARE) out |= 1u << 15;
    return out;
}

extern "C" void fp_touch_read(fp_touch_state* out) {
    out->buttons = g_buttons.load();
    out->lx = g_lx.load();
    out->ly = g_ly.load();
}
