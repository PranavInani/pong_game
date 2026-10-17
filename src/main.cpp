#include "raylib.h"
#include <cmath>

class Bar {
protected:
    Rectangle r;
    Color c;
    float speed = 8.0f;
    float screenHeight = (float)GetScreenHeight();
    float screenWidth = (float)GetScreenWidth();
    KeyboardKey up = KEY_W;
    KeyboardKey down = KEY_S;

public:
    Bar(float x, float y, float width, float height, Color col) {
        r = {x, y, width, height};
        c = col;
    }

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
        speed *= mul;
    }

    void draw() {
        DrawRectangleRec(r, c);
    }

    void move() {
        if (IsKeyDown(up)) r.y -= speed;
        if (IsKeyDown(down)) r.y += speed;

        // Keep the whole paddle inside the screen.
        if (r.y < 0) r.y = 0;
        if (r.y + r.height > screenHeight) r.y = screenHeight - r.height;
    }
};

class Ball {
protected:
    Vector2 screen = {(float)GetScreenWidth(), (float)GetScreenHeight()};
    float radius = 20.0f;
    Color c = RED;

public:
    Vector2 velocity;
    Vector2 center;

    Ball() {
        setPosition();
        setRandVelocity();
    }

    void draw() {
        DrawCircleV(center, radius, c);
    }

    void move() {
        center.x += velocity.x;
        center.y += velocity.y;
    }

    void setPosition(float x, float y) {
        center = {x, y};
    }

    void setPosition() {
        center = {screen.x / 2.0f, screen.y / 2.0f};
    }

    void setVelocity(float x, float y) {
        velocity = {x, y};
    }

    void setRandVelocity() {
        velocity.x = (float)GetRandomValue(5, 8) *
                     (GetRandomValue(0, 1) ? 1.0f : -1.0f);
        velocity.y = (float)GetRandomValue(2, 5) *
                     (GetRandomValue(0, 1) ? 1.0f : -1.0f);
    }

    // Add a small angle variation while preserving the outgoing direction.
    void varyVelocity() {
        const float oldX = velocity.x;
        const float oldY = velocity.y;
        const float speed = sqrtf(oldX * oldX + oldY * oldY);

        if (speed == 0.0f) return;

        float angle = atan2f(oldY, oldX);
        angle += GetRandomValue(-10, 10) * DEG2RAD;

        velocity.x = speed * cosf(angle);
        velocity.y = speed * sinf(angle);

        // Keep the direction of the bounce consistent.
        if (oldX != 0.0f && velocity.x * oldX < 0.0f) {
            velocity.x *= -1.0f;
        }
        if (oldY != 0.0f && velocity.y * oldY < 0.0f) {
            velocity.y *= -1.0f;
        }
    }

    int collide(const Bar& b1, const Bar& b2, Sound bar_ball, Sound bar_wall) {
        // Top wall: only bounce if moving upward into it.
        if (center.y - radius <= 0.0f && velocity.y < 0.0f) {
            center.y = radius;
            velocity.y *= -1.0f;
            varyVelocity();
            PlaySound(bar_wall);
            return 1;
        }

        // Bottom wall: only bounce if moving downward into it.
        if (center.y + radius >= screen.y && velocity.y > 0.0f) {
            center.y = screen.y - radius;
            velocity.y *= -1.0f;
            varyVelocity();
            PlaySound(bar_wall);
            return 1;
        }

        // Left paddle. It should bounce the ball only when the ball
        // is travelling left into the paddle.
        Rectangle left = b1.getConfig();
        if (CheckCollisionCircleRec(center, radius, left) && velocity.x < 0.0f) {
            center.x = left.x + left.width + radius;
            velocity.x = fabsf(velocity.x);
            varyVelocity();
            PlaySound(bar_ball);
            return 1;
        }

        // Right paddle. It should bounce the ball only when the ball
        // is travelling right into the paddle.
        Rectangle right = b2.getConfig();
        if (CheckCollisionCircleRec(center, radius, right) && velocity.x > 0.0f) {
            center.x = right.x - radius;
            velocity.x = -fabsf(velocity.x);
            varyVelocity();
            PlaySound(bar_ball);
            return 1;
        }

        // Scoring: check the ball's full extent, not just its centre.
        if (center.x + radius < 0.0f) return -1; // left side: right player scores
        if (center.x - radius > screen.x) return -2; // right side: left player scores

        return 0;
    }
};

class ComputerBar : public Bar {
    using Bar::Bar;

    float predictPos(const Ball& b) {
        if (b.velocity.x <= 0.0f) return r.y + r.height / 2.0f;

        float time = (screenWidth - b.center.x - 60.0f) / b.velocity.x;
        if (time < 0.0f) time = 0.0f;

        // Reflect the predicted vertical position between the top and bottom
        // walls, accounting for the ball's radius.
        const float minY = 20.0f;
        const float maxY = screenHeight - 20.0f;
        const float span = maxY - minY;
        if (span <= 0.0f) return screenHeight / 2.0f;

        float rawY = b.center.y + b.velocity.y * time - minY;
        float period = 2.0f * span;
        float folded = fmodf(rawY, period);
        if (folded < 0.0f) folded += period;
        if (folded > span) folded = period - folded;

        return minY + folded;
    }

public:
    float move(const Ball& b) {
        float predictedPos = predictPos(b);
        float paddleCenter = r.y + r.height / 2.0f;
        float diff = predictedPos - paddleCenter;

        if (fabsf(diff) <= speed) {
            r.y += diff;
        } else if (diff > 0.0f) {
            r.y += speed;
        } else {
            r.y -= speed;
        }

        if (r.y < 0.0f) r.y = 0.0f;
        if (r.y + r.height > screenHeight) r.y = screenHeight - r.height;

        return predictedPos;
    }
};

int main() {
    const int screenWidth = 800;
    const int screenHeight = 600;

    SetRandomSeed(400);
    InitWindow(screenWidth, screenHeight, "PingPong");
    SetTargetFPS(60);
    InitAudioDevice();

    Bar bar1(20, 20, 20, 80, DARKBLUE);
    ComputerBar bar2(screenWidth - 40, 20, 20, 80, DARKBLUE);
    bar2.setControls(KEY_UP, KEY_DOWN);

    Ball b;
    int gameState = 0;
    Vector2 score = {0, 0};

    Sound bar_ball = LoadSound("resources/bar_ball.ogg");
    Sound bar_wall = LoadSound("resources/bar_wall.ogg");

    while (!WindowShouldClose()) {
        if (gameState == 0) {
            if (IsKeyPressed(KEY_ENTER)) gameState = 1;
        } else if (gameState == 1) {
            bar1.move();
            bar2.move(b);
            b.move();

            int result = b.collide(bar1, bar2, bar_ball, bar_wall);
            if (result < 0) {
                b.setPosition();

                // Start the next rally towards the player who conceded.
                if (result == -1) {
                    score.y++;
                    b.setVelocity(-fabsf(b.velocity.x), b.velocity.y);
                } else {
                    score.x++;
                    b.setVelocity(fabsf(b.velocity.x), b.velocity.y);
                }

                // Reset vertical velocity to a fresh random value.
                float y = (float)GetRandomValue(2, 5) *
                          (GetRandomValue(0, 1) ? 1.0f : -1.0f);
                b.setVelocity(b.velocity.x, y);
            }

            if (score.x > 6 || score.y > 6) gameState = 2;
        }

        BeginDrawing();
        ClearBackground(RAYWHITE);

        if (gameState == 0) {
            DrawText("Press Enter to Start Playing.", screenWidth / 7, screenHeight / 2.5f, 30, BLACK);
        } else {
            if (gameState == 1) {
                bar1.draw();
                bar2.draw();
                b.draw();
            }

            DrawText(TextFormat("%i     :     %i", (int)score.x, (int)score.y),
                     screenWidth / 2.5f, screenHeight / 4.0f, 32, BLACK);

            if (gameState == 2) {
                if (score.y > score.x)
                    DrawText("GAME OVER", screenWidth / 3.0f, screenHeight / 2.5f, 40, RED);
                else
                    DrawText("YOU WIN!", screenWidth / 3.0f, screenHeight / 2.5f, 40, DARKGREEN);
            }
        }

        EndDrawing();
    }

    UnloadSound(bar_ball);
    UnloadSound(bar_wall);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
