#include "raylib.h"
#include "raymath.h"

#include <cmath>

namespace {

// ---------------------------------------------------------------------------
// Look and feel
// ---------------------------------------------------------------------------
constexpr int SCREEN_W = 900;
constexpr int SCREEN_H = 600;

constexpr Color BG_TOP{8, 10, 22, 255};
constexpr Color BG_BOTTOM{22, 14, 40, 255};
constexpr Color GRID_COLOR{90, 110, 175, 20};
constexpr Color NEON_CYAN{86, 232, 255, 255};
constexpr Color NEON_PINK{255, 92, 158, 255};
constexpr Color NEON_AMBER{255, 198, 108, 255};
constexpr Color INK{228, 234, 255, 255};

// ---------------------------------------------------------------------------
// Gameplay tuning
// ---------------------------------------------------------------------------
constexpr float PADDLE_W = 16.0f;
constexpr float PADDLE_H = 96.0f;
constexpr float PADDLE_MARGIN = 30.0f;
constexpr float BALL_RADIUS = 11.0f;

constexpr float BALL_SPEED_START = 400.0f;
constexpr float BALL_SPEED_MAX = 950.0f;
constexpr float MAX_BOUNCE_ANGLE = 0.95f; // radians, ~54 degrees off horizontal
constexpr int WIN_SCORE = 7;

// Player paddle. Heavy but steerable: it accelerates, keeps momentum and has a
// dash burst, so where you are *and* how fast you are moving both matter.
constexpr float PLAYER_MAX_SPEED = 620.0f;
constexpr float PLAYER_ACCEL = 4000.0f;
constexpr float PLAYER_FRICTION = 2900.0f;
constexpr float DASH_SPEED_MUL = 2.6f;
constexpr float DASH_TIME = 0.14f;
constexpr float DASH_COOLDOWN = 0.85f;
constexpr float DASH_COST = 34.0f;
constexpr float ENERGY_REGEN = 26.0f;

// CPU paddle. Deliberately slower than the player: sharp angles, spin and well
// timed dashes are what win rallies.
constexpr float CPU_MAX_SPEED = 500.0f;
constexpr float CPU_ACCEL = 2900.0f;

// ---------------------------------------------------------------------------
// Small math / drawing helpers
// ---------------------------------------------------------------------------
float ClampF(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

Color WithAlpha(Color color, float alpha) {
    color.a = (unsigned char)(ClampF(alpha, 0.0f, 1.0f) * 255.0f);
    return color;
}

Rectangle Inflate(Rectangle rec, float amount) {
    return Rectangle{rec.x - amount, rec.y - amount,
                     rec.width + amount * 2.0f, rec.height + amount * 2.0f};
}

// Fold a vertical position back and forth between two walls, the way a ball
// reflects off them. Shared by the CPU prediction and the impact marker.
float FoldY(float y, float minY, float maxY) {
    const float span = maxY - minY;
    if (span <= 0.0f) return minY;

    const float period = 2.0f * span;
    float folded = fmodf(y - minY, period);
    if (folded < 0.0f) folded += period;
    if (folded > span) folded = period - folded;

    return minY + folded;
}

// Keep the velocity shallow enough that the ball always makes progress left or
// right, no matter how much spin or how sharp an angle it picked up.
void ClampDirectionAngle(Vector2& velocity, float maxAngle) {
    const float speed = Vector2Length(velocity);
    if (speed <= 0.0001f) return;

    const float angle = atan2f(velocity.y, velocity.x);
    const float base = (cosf(angle) < 0.0f) ? PI : 0.0f;

    float delta = angle - base;
    while (delta > PI) delta -= 2.0f * PI;
    while (delta < -PI) delta += 2.0f * PI;
    delta = ClampF(delta, -maxAngle, maxAngle);

    velocity = {cosf(base + delta) * speed, sinf(base + delta) * speed};
}

// A rounded rectangle with a cheap neon bloom around it.
void DrawNeonRect(Rectangle rec, Color color, float spread, float roundness) {
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 4; i >= 1; --i) {
        const float t = (float)i / 4.0f;
        DrawRectangleRounded(Inflate(rec, spread * t), roundness, 12,
                             WithAlpha(color, 0.09f * (1.0f - t) + 0.02f));
    }
    EndBlendMode();

    DrawRectangleRounded(rec, roundness, 12, color);
    DrawRectangleRounded(Inflate(rec, -rec.width * 0.22f), roundness, 12,
                         WithAlpha(WHITE, 0.20f));
}

// Soft additive glow behind a circle (ball, particles, markers).
void DrawGlowCircle(Vector2 center, float radius, Color color, float alpha) {
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCircleGradient(center, radius, WithAlpha(color, alpha), BLANK);
    EndBlendMode();
}

void DrawGlowText(const char* text, Vector2 position, float size, Color color) {
    const Font font = GetFontDefault();
    const float spacing = size * 0.1f;

    BeginBlendMode(BLEND_ADDITIVE);
    const Color glow = WithAlpha(color, 0.16f);
    DrawTextEx(font, text, Vector2Add(position, Vector2{2.0f, 0.0f}), size, spacing, glow);
    DrawTextEx(font, text, Vector2Add(position, Vector2{-2.0f, 0.0f}), size, spacing, glow);
    DrawTextEx(font, text, Vector2Add(position, Vector2{0.0f, 2.0f}), size, spacing, glow);
    DrawTextEx(font, text, Vector2Add(position, Vector2{0.0f, -2.0f}), size, spacing, glow);
    DrawTextEx(font, text, position, size, spacing, glow);
    EndBlendMode();

    DrawTextEx(font, text, position, size, spacing, color);
}

void DrawGlowTextCentered(const char* text, float centerX, float y, float size, Color color) {
    const Vector2 measured = MeasureTextEx(GetFontDefault(), text, size, size * 0.1f);
    DrawGlowText(text, Vector2{centerX - measured.x / 2.0f, y}, size, color);
}

// ---------------------------------------------------------------------------
// Particles: sparks on paddle hits, walls and goals.
// ---------------------------------------------------------------------------
class ParticleSystem {
    static constexpr int MAX_PARTICLES = 512;

    struct Particle {
        Vector2 position{};
        Vector2 velocity{};
        float life = 0.0f;
        float maxLife = 1.0f;
        float size = 3.0f;
        Color color = WHITE;
    };

    Particle pool[MAX_PARTICLES];
    int active = 0;

public:
    void clear() {
        active = 0;
    }

    void spawn(Vector2 position, Vector2 velocity, float life, float size, Color color) {
        if (active >= MAX_PARTICLES) return;

        Particle& p = pool[active++];
        p.position = position;
        p.velocity = velocity;
        p.life = life;
        p.maxLife = life;
        p.size = size;
        p.color = color;
    }

    void burst(Vector2 position, Color color, int amount, float speed, float life) {
        for (int i = 0; i < amount; ++i) {
            const float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
            const float magnitude = speed * (0.35f + (float)GetRandomValue(0, 100) / 100.0f);
            const Vector2 velocity{cosf(angle) * magnitude, sinf(angle) * magnitude};
            const float particleLife = life * (0.55f + 0.9f * (float)GetRandomValue(0, 100) / 100.0f);
            spawn(position, velocity, particleLife, (float)GetRandomValue(2, 5), color);
        }
    }

    void update(float dt) {
        for (int i = 0; i < active;) {
            Particle& p = pool[i];
            p.life -= dt;

            if (p.life <= 0.0f) {
                pool[i] = pool[--active]; // swap-remove the dead particle
                continue;
            }

            p.velocity = Vector2Scale(p.velocity, 1.0f - 1.9f * dt); // drag
            p.position = Vector2Add(p.position, Vector2Scale(p.velocity, dt));
            ++i;
        }
    }

    void draw() const {
        BeginBlendMode(BLEND_ADDITIVE);
        for (int i = 0; i < active; ++i) {
            const Particle& p = pool[i];
            const float t = p.life / p.maxLife; // 1 -> 0 over the particle's life
            DrawCircleV(p.position, p.size * (0.35f + 0.65f * t), Fade(p.color, t));
        }
        EndBlendMode();
    }
};

} // namespace

class Ball;

// ---------------------------------------------------------------------------
// Bar: one paddle.
//
// Movement is momentum based instead of a constant teleport-speed, which turns
// positioning into a skill: you accelerate into a sprint, drift when you let go
// of the keys, and can spend energy on a short dash to cover a lot of ground.
// The speed a paddle is moving at when it strikes the ball is also what bends
// the return (see Ball::bounceOffPaddle).
// ---------------------------------------------------------------------------
class Bar {
protected:
    Rectangle r;
    Color c;
    Vector2 velocity{0.0f, 0.0f};

    float maxSpeed;
    float accel;
    float friction;

    KeyboardKey up = KEY_W;
    KeyboardKey down = KEY_S;
    KeyboardKey dashKey = KEY_SPACE;

    // Dash state.
    float energy = 100.0f;
    float dashTimeLeft = 0.0f;
    float dashCooldown = 0.0f;
    int dashDir = 0;
    int lastDir = 1; // remembered so an idle dash still has a direction
    bool dashing = false;
    bool dashFlash = false; // one-shot signal for effects

public:
    Bar(float x, float y, float width, float height, Color col, float speed, float acceleration)
        : r{x, y, width, height}, c(col), maxSpeed(speed), accel(acceleration),
          friction(speed * 4.7f) {}

    Rectangle getConfig() const {
        return r;
    }

    void setControls(KeyboardKey upper, KeyboardKey downn) {
        up = upper;
        down = downn;
    }

    void setSize(float width, float height) {
        if (width > 0) r.width = width;
        if (height > 0) r.height = height;
    }

    void setColor(Color col) {
        c = col;
    }

    void setSpeed(float mul) {
        maxSpeed *= mul;
    }

    // Which key fires this paddle's dash (KEY_NULL disables it, as for the CPU).
    void setDashKey(KeyboardKey key) {
        dashKey = key;
    }

    // Resets the dynamic state, used when a fresh match starts.
    void reset() {
        r.y = (float)SCREEN_H / 2.0f - r.height / 2.0f;
        velocity = {0.0f, 0.0f};
        energy = 100.0f;
        dashTimeLeft = 0.0f;
        dashCooldown = 0.0f;
        dashing = false;
    }

    float getVelocityY() const { return velocity.y; }
    float getMaxSpeed() const { return maxSpeed; }
    float getEnergy() const { return energy; }
    float getDashCooldown() const { return dashCooldown; }
    bool isDashing() const { return dashing; }

    bool dashReady() const {
        return dashTimeLeft <= 0.0f && dashCooldown <= 0.0f && energy >= DASH_COST;
    }

    // True once per dash, so the caller can spawn a burst of sparks.
    bool consumeDashFlash() {
        const bool flash = dashFlash;
        dashFlash = false;
        return flash;
    }

    void tryDash(float inputDir) {
        if (!dashReady()) return;

        dashDir = (inputDir != 0.0f) ? (inputDir < 0.0f ? -1 : 1) : lastDir;
        dashTimeLeft = DASH_TIME;
        dashCooldown = DASH_COOLDOWN;
        energy -= DASH_COST;
        dashing = true;
        dashFlash = true;
    }

    // Called when a new rally is served; the CPU uses it to re-roll its aim.
    virtual void onNewRally() {}

    virtual void update(const Ball& ball, float dt) {
        (void)ball; // the player paddle is driven purely by input

        float input = 0.0f;
        if (IsKeyDown(up)) input -= 1.0f;
        if (IsKeyDown(down)) input += 1.0f;
        if (input != 0.0f) lastDir = (input < 0.0f) ? -1 : 1;

        if (IsKeyPressed(dashKey)) tryDash(input);

        if (dashTimeLeft > 0.0f) {
            dashTimeLeft -= dt;
            velocity.y = (float)dashDir * maxSpeed * DASH_SPEED_MUL;
            if (dashTimeLeft <= 0.0f) {
                dashing = false;
                velocity.y = ClampF(velocity.y, -maxSpeed, maxSpeed);
            }
        } else if (input != 0.0f) {
            // Holding a key builds speed up to the cap.
            velocity.y = ClampF(velocity.y + input * accel * dt, -maxSpeed, maxSpeed);
        } else {
            // Releasing keys lets the paddle coast to a stop.
            const float drop = friction * dt;
            if (fabsf(velocity.y) <= drop) velocity.y = 0.0f;
            else velocity.y -= (velocity.y > 0.0f) ? drop : -drop;
        }

        r.y += velocity.y * dt;
        clampToCourt();
        updateTimers(dt);
    }

    virtual void draw() const {
        const Color body = dashing ? WHITE : c;
        DrawNeonRect(r, body, dashing ? 16.0f : 10.0f, 0.6f);
    }

protected:
    void clampToCourt() {
        const float bottom = (float)SCREEN_H - r.height;
        if (r.y < 0.0f) {
            r.y = 0.0f;
            if (velocity.y < 0.0f) velocity.y = 0.0f;
        } else if (r.y > bottom) {
            r.y = bottom;
            if (velocity.y > 0.0f) velocity.y = 0.0f;
        }
    }

    void updateTimers(float dt) {
        if (dashCooldown > 0.0f) dashCooldown = fmaxf(0.0f, dashCooldown - dt);
        if (!dashing) energy = fminf(100.0f, energy + ENERGY_REGEN * dt);
    }
};

// ---------------------------------------------------------------------------
// What a single frame of collision produced.
// ---------------------------------------------------------------------------
struct HitInfo {
    bool hit = false;   // the ball touched something
    bool wall = false;  // ...a wall
    bool paddle = false; // ...a paddle
    bool powered = false; // ...and that paddle was dashing
    int scorer = 0;     // 1 = left player scored, 2 = right player scored
    Vector2 point{};    // where the contact happened
    float sharpness = 0.0f; // -1 = top edge of the paddle, +1 = bottom edge
};

// ---------------------------------------------------------------------------
// Ball.
//
// Two skills are available to both sides:
//   * where you hit it  -> the return angle (edge hits are sharp, centre is flat)
//   * how you were moving -> a leaning paddle adds spin, and spin curves the
//     flight, so you can bend the ball around your opponent.
// ---------------------------------------------------------------------------
class Ball {
    static constexpr int TRAIL_LENGTH = 22;

    float radius = BALL_RADIUS;
    float speed = BALL_SPEED_START;
    float spin = 0.0f; // radians per second added to the flight direction
    Color c = NEON_AMBER;

    Vector2 trail[TRAIL_LENGTH];
    int trailCursor = 0;
    float pulse = 0.0f;

public:
    Vector2 center{SCREEN_W / 2.0f, SCREEN_H / 2.0f};
    Vector2 velocity{0.0f, 0.0f};

    Ball() {
        resetTrail();
    }

    float getRadius() const { return radius; }
    float getSpeed() const { return speed; }
    float getSpin() const { return spin; }
    Color getColor() const { return c; }

    void resetTrail() {
        for (int i = 0; i < TRAIL_LENGTH; ++i) trail[i] = center;
        trailCursor = 0;
    }

    // Park the ball in the middle while a serve is being counted in.
    void hold() {
        center = {SCREEN_W / 2.0f, SCREEN_H / 2.0f};
        velocity = {0.0f, 0.0f};
        speed = BALL_SPEED_START;
        spin = 0.0f;
        resetTrail();
    }

    // dirX: +1 serves to the right, -1 serves to the left.
    void launch(int dirX) {
        const float angle = (float)GetRandomValue(-30, 30) * DEG2RAD;
        velocity = {cosf(angle) * speed * (dirX > 0 ? 1.0f : -1.0f), sinf(angle) * speed};
        spin = 0.0f;
        resetTrail();
    }

    void setPosition(float x, float y) {
        center = {x, y};
    }

    void setPosition() {
        center = {SCREEN_W / 2.0f, SCREEN_H / 2.0f};
    }

    void setVelocity(float x, float y) {
        velocity = {x, y};
        speed = Vector2Length(velocity);
    }

    void setRandVelocity() {
        velocity.x = (float)GetRandomValue(5, 8) * (GetRandomValue(0, 1) ? 1.0f : -1.0f);
        velocity.y = (float)GetRandomValue(2, 5) * (GetRandomValue(0, 1) ? 1.0f : -1.0f);
        speed = Vector2Length(velocity);
    }

    void update(float dt) {
        pulse += dt;

        // Spin bends the flight path and bleeds off over time.
        if (fabsf(spin) > 0.0005f) {
            const float angle = atan2f(velocity.y, velocity.x) + spin * dt;
            velocity = {cosf(angle) * speed, sinf(angle) * speed};
            spin *= expf(-1.4f * dt);
            ClampDirectionAngle(velocity, MAX_BOUNCE_ANGLE);
        }

        center = Vector2Add(center, Vector2Scale(velocity, dt));

        trail[trailCursor] = center;
        trailCursor = (trailCursor + 1) % TRAIL_LENGTH;
    }

    int collide(const Bar& left, const Bar& right) {
        return step(left, right).scorer;
    }

    // One collision pass: walls first, then paddles, then the goal lines.
    HitInfo step(const Bar& left, const Bar& right) {
        HitInfo info;

        if (center.y - radius <= 0.0f && velocity.y < 0.0f) {
            center.y = radius;
            velocity.y = -velocity.y;
            spin *= 0.6f;
            ClampDirectionAngle(velocity, MAX_BOUNCE_ANGLE);
            info.hit = true;
            info.wall = true;
            info.point = center;
            return info;
        }

        if (center.y + radius >= (float)SCREEN_H && velocity.y > 0.0f) {
            center.y = (float)SCREEN_H - radius;
            velocity.y = -velocity.y;
            spin *= 0.6f;
            ClampDirectionAngle(velocity, MAX_BOUNCE_ANGLE);
            info.hit = true;
            info.wall = true;
            info.point = center;
            return info;
        }

        const Rectangle leftRec = left.getConfig();
        if (velocity.x < 0.0f && center.x > leftRec.x &&
            CheckCollisionCircleRec(center, radius, leftRec)) {
            bounceOffPaddle(left, 1.0f, info);
            return info;
        }

        const Rectangle rightRec = right.getConfig();
        if (velocity.x > 0.0f && center.x < rightRec.x + rightRec.width &&
            CheckCollisionCircleRec(center, radius, rightRec)) {
            bounceOffPaddle(right, -1.0f, info);
            return info;
        }

        if (center.x + radius < 0.0f) info.scorer = 2;             // player conceded
        else if (center.x - radius > (float)SCREEN_W) info.scorer = 1; // CPU conceded

        return info;
    }

    void draw() const {
        // Motion trail, oldest and faintest first.
        for (int i = 0; i < TRAIL_LENGTH; ++i) {
            const int index = (trailCursor + i) % TRAIL_LENGTH;
            const float t = (float)(i + 1) / TRAIL_LENGTH;
            DrawCircleV(trail[index], radius * (0.25f + 0.65f * t),
                        WithAlpha(c, 0.05f + 0.16f * t * t));
        }

        const float breathing = 1.0f + 0.06f * sinf(pulse * 4.0f);
        DrawGlowCircle(center, radius * 3.4f * breathing, c, 0.26f);
        DrawCircleV(center, radius * breathing, c);
        DrawCircleV(center, radius * breathing * 0.55f, WithAlpha(WHITE, 0.9f));
    }

    // Where the ball will be when its centre reaches targetX, folded off the
    // walls. The CPU uses this to line up its return.
    float predictYAt(float targetX) const {
        if (fabsf(velocity.x) < 0.0001f) return center.y;

        const float time = (targetX - center.x) / velocity.x;
        if (time < 0.0f) return center.y;

        return FoldY(center.y + velocity.y * time, radius, (float)SCREEN_H - radius);
    }

private:
    void bounceOffPaddle(const Bar& paddle, float dirX, HitInfo& info) {
        const Rectangle rec = paddle.getConfig();
        const float paddleCentre = rec.y + rec.height * 0.5f;
        const float range = rec.height * 0.5f + radius;

        // -1 at the top edge, 0 in the middle, +1 at the bottom edge.
        const float rel = ClampF((center.y - paddleCentre) / range, -1.0f, 1.0f);

        // How fast the paddle was travelling, as a -1..1 factor.
        const float lean = ClampF(paddle.getVelocityY() / paddle.getMaxSpeed(), -1.0f, 1.0f);
        const bool powered = paddle.isDashing();

        // Edge hits give a sharp angle, a moving paddle bends it further.
        float angle = rel * MAX_BOUNCE_ANGLE + lean * 0.32f;
        angle = ClampF(angle, -MAX_BOUNCE_ANGLE, MAX_BOUNCE_ANGLE);

        // Every paddle hit speeds the rally up, a dash hit speeds it up more.
        speed = fminf(speed * 1.045f + 9.0f, BALL_SPEED_MAX);
        if (powered) speed = fminf(speed + 55.0f, BALL_SPEED_MAX + 55.0f);

        velocity = {cosf(angle) * speed * dirX, sinf(angle) * speed};
        spin = ClampF(lean * 1.5f, -1.5f, 1.5f);

        // Shove the ball clear of the paddle so it can never get stuck.
        center.x = (dirX > 0.0f) ? rec.x + rec.width + radius : rec.x - radius;

        info.hit = true;
        info.paddle = true;
        info.powered = powered;
        info.sharpness = rel;
        info.point = {center.x, center.y};
    }
};

// ---------------------------------------------------------------------------
// ComputerBar: the opponent (prediction logic based on Vedant Sharma's work).
// It runs the same ball maths as the player but has a lower top speed and a
// small per-rally misjudgement, so well placed shots and dashes can beat it.
// ---------------------------------------------------------------------------
class ComputerBar : public Bar {
    float aimError = 0.0f; // per-rally misjudgement, in pixels
    float noisePhase = 0.0f;

    float predictPos(const Ball& b) const {
        if (b.velocity.x <= 0.0f) return r.y + r.height * 0.5f;
        return b.predictYAt(r.x);
    }

public:
    ComputerBar(float x, float y, float width, float height, Color col, float speed)
        : Bar(x, y, width, height, col, speed, CPU_ACCEL) {
        dashKey = KEY_NULL; // the CPU never dashes
    }

    void onNewRally() override {
        aimError = (float)GetRandomValue(-64, 64);
        noisePhase = 0.0f;
    }

    void update(const Ball& ball, float dt) override {
        float target;
        if (ball.velocity.x > 0.0f) {
            // Ball is heading this way: chase where it will cross the paddle.
            noisePhase += dt * 3.0f;
            target = predictPos(ball) + aimError + sinf(noisePhase) * 12.0f;
        } else {
            // Otherwise drift back toward the middle, ready for the next shot.
            noisePhase = 0.0f;
            target = (float)SCREEN_H / 2.0f;
        }

        target = ClampF(target, r.height * 0.5f, (float)SCREEN_H - r.height * 0.5f);

        // Proportional controller: it eases into the target instead of jittering
        // around it, and its capped speed is what the player has to outplay.
        const float centre = r.y + r.height * 0.5f;
        const float desired = ClampF((target - centre) * 9.0f, -maxSpeed, maxSpeed);
        velocity.y += ClampF(desired - velocity.y, -accel * dt, accel * dt);

        r.y += velocity.y * dt;
        clampToCourt();
    }
};

// ---------------------------------------------------------------------------
// Background, court and HUD
// ---------------------------------------------------------------------------
void DrawBackground(float elapsed) {
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, BG_TOP, BG_BOTTOM);

    // Slow drifting blooms of colour behind everything.
    const Vector2 blobs[3] = {
        {SCREEN_W * 0.22f, SCREEN_H * 0.30f},
        {SCREEN_W * 0.78f, SCREEN_H * 0.68f},
        {SCREEN_W * 0.50f, SCREEN_H * 0.12f},
    };
    const Color colors[3] = {NEON_CYAN, NEON_PINK, NEON_AMBER};

    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < 3; ++i) {
        const float drift = sinf(elapsed * 0.25f + (float)i * 2.1f) * 28.0f;
        const Vector2 p = {blobs[i].x, blobs[i].y + drift};
        DrawCircleGradient(p, 270.0f, WithAlpha(colors[i], 0.05f), BLANK);
    }
    EndBlendMode();

    for (int x = 0; x <= SCREEN_W; x += 45) DrawLine(x, 0, x, SCREEN_H, GRID_COLOR);
    for (int y = 0; y <= SCREEN_H; y += 45) DrawLine(0, y, SCREEN_W, y, GRID_COLOR);
}

// Drawn inside the shaken world, so the court moves with the ball impacts.
void DrawCourt(float elapsed) {
    const float dash = 20.0f;
    const float gap = 16.0f;
    for (float y = gap * 0.5f; y < (float)SCREEN_H; y += dash + gap) {
        const float shimmer = 0.06f + 0.03f * sinf(elapsed * 2.0f + y * 0.05f);
        DrawRectangle(SCREEN_W / 2 - 2, (int)y, 4, (int)dash, WithAlpha(INK, shimmer));
    }

    const Rectangle border = {4.0f, 4.0f, (float)SCREEN_W - 8.0f, (float)SCREEN_H - 8.0f};
    BeginBlendMode(BLEND_ADDITIVE);
    DrawRectangleRoundedLinesEx(Inflate(border, 2.5f), 0.04f, 8, 5.0f, WithAlpha(NEON_CYAN, 0.05f));
    EndBlendMode();
    DrawRectangleRoundedLinesEx(border, 0.04f, 8, 2.0f, WithAlpha(NEON_CYAN, 0.22f));
}

// Big, translucent numerals painted on each half of the court.
void DrawGhostScores(int playerScore, int cpuScore) {
    const char* leftText = TextFormat("%i", playerScore);
    const char* rightText = TextFormat("%i", cpuScore);

    Vector2 size = MeasureTextEx(GetFontDefault(), leftText, 210.0f, 21.0f);
    DrawTextEx(GetFontDefault(), leftText,
               Vector2{SCREEN_W * 0.25f - size.x / 2.0f, SCREEN_H * 0.5f - size.y / 2.0f},
               210.0f, 21.0f, WithAlpha(NEON_CYAN, 0.10f));

    size = MeasureTextEx(GetFontDefault(), rightText, 210.0f, 21.0f);
    DrawTextEx(GetFontDefault(), rightText,
               Vector2{SCREEN_W * 0.75f - size.x / 2.0f, SCREEN_H * 0.5f - size.y / 2.0f},
               210.0f, 21.0f, WithAlpha(NEON_PINK, 0.10f));
}

void DrawEnergyBar(Rectangle area, float energy01, float cooldown01, bool ready) {
    DrawRectangleRounded(area, 0.5f, 8, WithAlpha(BLACK, 0.45f));

    Rectangle fill = area;
    fill.width = area.width * ClampF(energy01, 0.0f, 1.0f);
    if (fill.width > 2.0f) {
        const Color tint = ready ? NEON_CYAN : NEON_AMBER;
        DrawRectangleRounded(fill, 0.5f, 8, WithAlpha(tint, ready ? 0.95f : 0.6f));

        BeginBlendMode(BLEND_ADDITIVE);
        DrawRectangleRounded(Inflate(fill, 4.0f), 0.5f, 8, WithAlpha(tint, 0.10f));
        EndBlendMode();
    }

    if (!ready && cooldown01 > 0.0f) {
        // Sweep mark showing how much of the dash cooldown is left.
        const float x = area.x + area.width * (1.0f - ClampF(cooldown01, 0.0f, 1.0f));
        DrawRectangle((int)x - 1, (int)area.y - 3, 2, (int)area.height + 6,
                      WithAlpha(NEON_AMBER, 0.5f));
    }

    DrawRectangleRoundedLinesEx(area, 0.5f, 8, 1.0f, WithAlpha(INK, 0.18f));
}

// Draws a "DASH <state>" label left-aligned above a bar, or right-aligned when
// rightAlign is set (used for player 2 on the other side of the court).
void DrawDashLabel(float x, float y, const Bar& bar, Color tint, bool rightAlign) {
    const Font font = GetFontDefault();
    const char* state = bar.dashReady() ? "READY" : "CHARGING";
    const float labelWidth = MeasureTextEx(font, "DASH", 16.0f, 1.6f).x;
    const float stateWidth = MeasureTextEx(font, state, 16.0f, 1.6f).x;
    const float gap = 14.0f;

    const float startX = rightAlign ? x - (labelWidth + gap + stateWidth) : x;

    DrawTextEx(font, "DASH", Vector2{startX, y}, 16.0f, 1.6f, WithAlpha(INK, 0.55f));
    DrawTextEx(font, state, Vector2{startX + labelWidth + gap, y}, 16.0f, 1.6f,
               WithAlpha(bar.dashReady() ? tint : NEON_AMBER, 0.75f));
}

void DrawHud(int leftScore, int rightScore, const Bar& left, const Bar* right, bool twoPlayer) {
    DrawGlowTextCentered(twoPlayer ? "P1" : "YOU", SCREEN_W * 0.5f - 130.0f, 22.0f, 22.0f,
                         WithAlpha(NEON_CYAN, 0.85f));
    DrawGlowTextCentered(twoPlayer ? "P2" : "CPU", SCREEN_W * 0.5f + 130.0f, 22.0f, 22.0f,
                         WithAlpha(NEON_PINK, 0.85f));

    const char* scores = TextFormat("%i     :     %i", leftScore, rightScore);
    DrawGlowTextCentered(scores, (float)SCREEN_W / 2.0f, 16.0f, 32.0f, INK);

    const float labelY = (float)SCREEN_H - 66.0f;
    const float barY = (float)SCREEN_H - 44.0f;

    const Rectangle leftBar{30.0f, barY, 190.0f, 12.0f};
    DrawEnergyBar(leftBar, left.getEnergy() / 100.0f, left.getDashCooldown() / DASH_COOLDOWN,
                  left.dashReady());
    DrawDashLabel(30.0f, labelY, left, NEON_CYAN, false);

    if (right != nullptr) {
        const Rectangle rightBar{(float)SCREEN_W - 220.0f, barY, 190.0f, 12.0f};
        DrawEnergyBar(rightBar, right->getEnergy() / 100.0f,
                      right->getDashCooldown() / DASH_COOLDOWN, right->dashReady());
        DrawDashLabel((float)SCREEN_W - 30.0f, labelY, *right, NEON_PINK, true);
    }

    if (twoPlayer) {
        DrawGlowTextCentered("W / S  +  SPACE", SCREEN_W * 0.37f, labelY, 16.0f,
                             WithAlpha(INK, 0.40f));
        DrawGlowTextCentered("ARROWS  +  R SHIFT", SCREEN_W * 0.63f, labelY, 16.0f,
                             WithAlpha(INK, 0.40f));
    } else {
        DrawTextEx(GetFontDefault(), "W / S move     SPACE dash     P pause",
                   Vector2{(float)SCREEN_W - 430.0f, labelY}, 16.0f, 1.6f,
                   WithAlpha(INK, 0.35f));
    }
}

// ---------------------------------------------------------------------------
// Screens drawn on top of the court
// ---------------------------------------------------------------------------
enum class GameState { Menu, Serve, Play, Point, GameOver };

void DrawMenuOverlay(float elapsed) {
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, WithAlpha(BG_TOP, 0.62f));

    DrawGlowTextCentered("NEON PONG", (float)SCREEN_W / 2.0f, 78.0f, 70.0f, NEON_CYAN);
    DrawGlowTextCentered("a pong match of angles, spin and dashes",
                         (float)SCREEN_W / 2.0f, 160.0f, 20.0f, WithAlpha(INK, 0.7f));

    const char* lines[] = {
        "W / S       move paddle   (it accelerates and coasts)",
        "SPACE       dash burst    (costs energy)",
        "EDGE HITS   sharper return angle",
        "MOVE + HIT  adds spin, the ball curves",
        "P / ESC     pause / quit",
    };
    const int lineCount = (int)(sizeof(lines) / sizeof(lines[0]));
    for (int i = 0; i < lineCount; ++i) {
        DrawGlowTextCentered(lines[i], (float)SCREEN_W / 2.0f, 210.0f + (float)i * 28.0f, 20.0f,
                             WithAlpha(INK, 0.85f));
    }

    const float pulse = 0.5f + 0.5f * sinf(elapsed * 3.2f);

    DrawGlowTextCentered("PRESS 1  -  ONE PLAYER   (you vs CPU)", (float)SCREEN_W / 2.0f, 402.0f,
                         24.0f, WithAlpha(NEON_CYAN, 0.55f + 0.4f * pulse));

    DrawGlowTextCentered("PRESS 2  -  TWO PLAYERS   (W / S   vs   ARROWS)", (float)SCREEN_W / 2.0f,
                         440.0f, 24.0f, WithAlpha(NEON_PINK, 0.55f + 0.4f * pulse));

    DrawGlowTextCentered("player 1 dashes with SPACE  -  player 2 dashes with RIGHT SHIFT",
                         (float)SCREEN_W / 2.0f, 486.0f, 17.0f, WithAlpha(INK, 0.5f));
    DrawGlowTextCentered("first to 7 wins", (float)SCREEN_W / 2.0f, 514.0f, 17.0f,
                         WithAlpha(INK, 0.5f));
}

void DrawGameOverOverlay(float elapsed, int leftScore, int rightScore, bool twoPlayer) {
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, WithAlpha(BG_TOP, 0.74f));

    const bool leftWon = leftScore > rightScore;
    const char* title = twoPlayer ? (leftWon ? "PLAYER 1 WINS" : "PLAYER 2 WINS")
                                  : (leftWon ? "YOU WIN!" : "CPU WINS");
    DrawGlowTextCentered(title, (float)SCREEN_W / 2.0f, 170.0f, 58.0f,
                         leftWon ? NEON_CYAN : NEON_PINK);

    const char* score = TextFormat("%i   -   %i", leftScore, rightScore);
    DrawGlowTextCentered(score, (float)SCREEN_W / 2.0f, 252.0f, 34.0f, WithAlpha(INK, 0.8f));

    const float pulse = 0.5f + 0.5f * sinf(elapsed * 3.2f);
    DrawGlowTextCentered("ENTER  -  rematch", (float)SCREEN_W / 2.0f, 352.0f, 24.0f,
                         WithAlpha(NEON_AMBER, 0.45f + 0.5f * pulse));
    DrawGlowTextCentered("M  -  back to menu      ESC  -  quit", (float)SCREEN_W / 2.0f, 394.0f,
                         20.0f, WithAlpha(INK, 0.55f));
}

void DrawPauseOverlay() {
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, WithAlpha(BG_TOP, 0.62f));
    DrawGlowTextCentered("PAUSED", (float)SCREEN_W / 2.0f, 236.0f, 56.0f, NEON_AMBER);
    DrawGlowTextCentered("press P to resume", (float)SCREEN_W / 2.0f, 312.0f, 22.0f,
                         WithAlpha(INK, 0.7f));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Neon Pong");
    SetTargetFPS(60);
    SetTextureFilter(GetFontDefault().texture, TEXTURE_FILTER_BILINEAR);

    InitAudioDevice();
    const bool audioReady = IsAudioDeviceReady();

    Sound paddleSound{};
    Sound wallSound{};
    if (audioReady) {
        paddleSound = LoadSound("resources/bar_ball.ogg");
        wallSound = LoadSound("resources/bar_wall.ogg");
        SetSoundVolume(paddleSound, 0.7f);
        SetSoundVolume(wallSound, 0.45f);
    }

    Bar player(PADDLE_MARGIN, (float)SCREEN_H / 2.0f - PADDLE_H / 2.0f, PADDLE_W, PADDLE_H,
               NEON_CYAN, PLAYER_MAX_SPEED, PLAYER_ACCEL);
    player.setControls(KEY_W, KEY_S);
    player.setDashKey(KEY_SPACE);

    Bar player2((float)SCREEN_W - PADDLE_MARGIN - PADDLE_W,
                (float)SCREEN_H / 2.0f - PADDLE_H / 2.0f, PADDLE_W, PADDLE_H, NEON_PINK,
                PLAYER_MAX_SPEED, PLAYER_ACCEL);
    player2.setControls(KEY_UP, KEY_DOWN);
    player2.setDashKey(KEY_RIGHT_SHIFT);

    ComputerBar cpu((float)SCREEN_W - PADDLE_MARGIN - PADDLE_W,
                    (float)SCREEN_H / 2.0f - PADDLE_H / 2.0f, PADDLE_W, PADDLE_H, NEON_PINK,
                    CPU_MAX_SPEED);

    Ball ball;
    ParticleSystem particles;
    Camera2D camera{};
    camera.offset = {0.0f, 0.0f};
    camera.target = {0.0f, 0.0f};
    camera.zoom = 1.0f;

    GameState state = GameState::Menu;
    int leftScore = 0;
    int rightScore = 0;
    int serveDir = 1;
    float stateTimer = 0.0f;
    float elapsed = 0.0f;
    float shake = 0.0f;
    float hitFlash = 0.0f;
    bool paused = false;
    bool twoPlayer = false;

    // The right hand paddle is either the second player or the CPU.
    Bar* right = &cpu;

    auto beginRally = [&]() {
        ball.hold();
        right->onNewRally();
        state = GameState::Serve;
        stateTimer = 0.0f;
    };

    auto startMatch = [&](bool versusHuman) {
        twoPlayer = versusHuman;
        right = versusHuman ? static_cast<Bar*>(&player2) : static_cast<Bar*>(&cpu);

        leftScore = 0;
        rightScore = 0;
        serveDir = 1;
        player.reset();
        player2.reset();
        cpu.reset();
        particles.clear();
        shake = 0.0f;
        beginRally();
    };

    while (!WindowShouldClose()) {
        // Clamp dt so a hitch can never let the ball tunnel through a paddle.
        const float dt = fminf(GetFrameTime(), 1.0f / 30.0f);
        elapsed += dt;

        const bool inRally = state == GameState::Serve || state == GameState::Play ||
                             state == GameState::Point;
        if (inRally && IsKeyPressed(KEY_P)) paused = !paused;

        if (!paused) {
            if (state == GameState::Menu) {
                if (IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_ENTER)) startMatch(false);
                else if (IsKeyPressed(KEY_TWO)) startMatch(true);
            } else if (state == GameState::Serve) {
                player.update(ball, dt);
                right->update(ball, dt);
                ball.update(dt);

                stateTimer += dt;
                if (stateTimer >= 0.75f) {
                    ball.launch(serveDir);
                    state = GameState::Play;
                }
            } else if (state == GameState::Play) {
                player.update(ball, dt);
                right->update(ball, dt);
                ball.update(dt);

                const HitInfo hit = ball.step(player, *right);
                if (hit.wall) {
                    shake = fmaxf(shake, 2.0f);
                    particles.burst(hit.point, WithAlpha(INK, 0.9f), 6, 130.0f, 0.35f);
                    if (audioReady) {
                        SetSoundPitch(wallSound, 0.95f + (float)GetRandomValue(0, 20) / 100.0f);
                        PlaySound(wallSound);
                    }
                } else if (hit.paddle) {
                    const bool onPlayerSide = ball.center.x < (float)SCREEN_W / 2.0f;
                    const Color spark = hit.powered ? NEON_AMBER : (onPlayerSide ? NEON_CYAN : NEON_PINK);

                    shake = fmaxf(shake, hit.powered ? 10.0f : 3.0f + 4.5f * fabsf(hit.sharpness));
                    hitFlash = fmaxf(hitFlash, hit.powered ? 1.0f : 0.45f);
                    particles.burst(hit.point, spark, hit.powered ? 26 : 14,
                                    hit.powered ? 270.0f : 190.0f, 0.5f);

                    if (audioReady) {
                        const float ramp = (ball.getSpeed() - BALL_SPEED_START) /
                                           (BALL_SPEED_MAX - BALL_SPEED_START);
                        SetSoundPitch(paddleSound, 0.9f + 0.6f * ClampF(ramp, 0.0f, 1.0f));
                        PlaySound(paddleSound);
                    }
                }

                if (hit.scorer != 0) {
                    if (hit.scorer == 1) leftScore++;
                    else rightScore++;

                    // Serve toward whoever just conceded.
                    serveDir = (hit.scorer == 1) ? 1 : -1;

                    const Vector2 goal{hit.scorer == 1 ? (float)SCREEN_W + 8.0f : -8.0f, ball.center.y};
                    particles.burst(goal, hit.scorer == 1 ? NEON_CYAN : NEON_PINK, 70, 340.0f, 0.9f);
                    shake = 13.0f;
                    hitFlash = 0.7f;

                    state = GameState::Point;
                    stateTimer = 0.0f;
                }
            } else if (state == GameState::Point) {
                player.update(ball, dt);
                right->update(ball, dt);

                stateTimer += dt;
                if (stateTimer >= 0.9f) {
                    if (leftScore >= WIN_SCORE || rightScore >= WIN_SCORE) {
                        state = GameState::GameOver;
                        stateTimer = 0.0f;
                    } else {
                        beginRally();
                    }
                }
            } else if (state == GameState::GameOver) {
                if (IsKeyPressed(KEY_ENTER)) startMatch(twoPlayer);
                else if (IsKeyPressed(KEY_M)) state = GameState::Menu;
            }

            // Dash bursts: one for each human paddle that just fired one.
            auto dashSpark = [&](Bar& bar, Color tint) {
                if (!bar.consumeDashFlash()) return;
                const Rectangle rec = bar.getConfig();
                particles.burst({rec.x + rec.width * 0.5f, rec.y + rec.height * 0.5f}, tint, 20,
                                210.0f, 0.4f);
                shake = fmaxf(shake, 3.5f);
            };
            dashSpark(player, NEON_CYAN);
            if (twoPlayer) dashSpark(player2, NEON_PINK);

            particles.update(dt);
            shake = fmaxf(0.0f, shake - dt * 28.0f);
            hitFlash = fmaxf(0.0f, hitFlash - dt * 2.4f);
        }

        // F12 grabs a frame, handy for sharing a good rally.
        if (IsKeyPressed(KEY_F12)) {
            static int shotCount = 0;
            TakeScreenshot(TextFormat("screenshot_%02i.png", ++shotCount));
        }

        camera.offset = {(float)GetRandomValue(-100, 100) / 100.0f * shake,
                         (float)GetRandomValue(-100, 100) / 100.0f * shake};

        // -------------------------------------------------------------------
        // Draw
        // -------------------------------------------------------------------
        BeginDrawing();
        ClearBackground(BG_TOP);

        DrawBackground(elapsed);

        BeginMode2D(camera);
        DrawCourt(elapsed);

        if (state == GameState::Menu) {
            // Attract screen: both paddles and the ball sit on the centre line.
            ball.draw();
            player2.draw();
            player.draw();
            particles.draw();
        } else if (state == GameState::GameOver) {
            // The court stays visible behind the result screen.
        } else {
            DrawGhostScores(leftScore, rightScore);
            if (state != GameState::Point) ball.draw();
            right->draw();
            player.draw();
            particles.draw();
        }
        EndMode2D();

        if (inRally) {
            DrawHud(leftScore, rightScore, player, twoPlayer ? &player2 : nullptr, twoPlayer);
        }

        if (hitFlash > 0.001f) {
            BeginBlendMode(BLEND_ADDITIVE);
            DrawRectangle(0, 0, SCREEN_W, SCREEN_H, WithAlpha(INK, hitFlash * 0.05f));
            EndBlendMode();
        }

        if (state == GameState::Menu) DrawMenuOverlay(elapsed);
        else if (state == GameState::GameOver) DrawGameOverOverlay(elapsed, leftScore, rightScore, twoPlayer);
        else if (paused) DrawPauseOverlay();

        EndDrawing();
    }

    if (audioReady) {
        UnloadSound(paddleSound);
        UnloadSound(wallSound);
    }
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
