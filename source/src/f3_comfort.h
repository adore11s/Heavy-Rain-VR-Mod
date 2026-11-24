#pragma once
// 1.7.1 (owner 2026-11-24: "make f3 less nauseating ... its too precise rn and its making me motion sick cuz its like
// capturing every yaw movement ... maybe just do smoothing"). F3 = the cutscene first-person view on the character's
// eyes (fpcam.cpp). Until now its heading followed the animated face with tau 0.15 s and its pitch followed the face's
// elevation: every glance and nod of the animation turned the player's world. DATA (tools/f3_fixture.py, 8 real cutscene
// clips of the four playable characters): the face alone turns 7 .. 35 deg/s on average and nods over 20 .. 120 deg.
// Now (playbook 13.2 too: authored pitch kept out by default, cuts snap):
// - heading: a LAZY follow - inside a dead zone the view stays still (glances, talking heads); beyond it the view eases
//   toward the face (tau) until it is within `settle`; a jump larger than `cut` in one frame (a camera cut, a teleport)
//   snaps at once;
// - pitch: the face's elevation only when fp_f3_pitch is on (then slowly), else level;
// - eye point: a light low-pass against head bob, snapping on jumps.
// 1.7.2 (owner, after the 1.7.1 run: "when i was in car the first person wasnt cathing up and i saw the body from
// behind ... i think i only like in the new f3 code that the camera doesnt go up and down"): a low-pass on a MOVING eye
// trails it by speed x tau (a car at 15 m/s: 1.8 m behind the head - the body seen from behind). Defaults now: the
// 1.7 heading (the face, tau 0.15 s, no dead zone), the eye point exactly the animation's (no smoothing), and only the
// pitch kept level (fp_f3_pitch off). The lazy torso follow stays as an option (fp_f3_follow=torso, TorsoParams).
// Pure: tests/f3_comfort_tests.cpp runs it on the real clips.
#include <cmath>

namespace hrvr::f3 {

struct Params {
    float deadzone = 0.f;    // rad: sway inside it does not turn the view (0: always follows, as 1.7)
    float settle = 0.f;      // rad: a catch-up ends this close (0: never ends - a plain ease, as 1.7)
    float tau = 0.15f;       // s: ease speed (1.7)
    float cut = 1.05f;       // rad (60 deg) in ONE frame: a cut, snap
    float pitchTau = 1.0f;   // s: elevation easing when fp_f3_pitch is on
    float posTau = 0.f;      // s: eye point low-pass (0: none - the eye is where the animation has it)
    float posCut = 0.5f;     // m in one frame: snap
};
// fp_f3_follow=torso (1.7.1's lazy follow, without the eye low-pass)
inline Params TorsoParams() {
    Params p;
    p.deadzone = 0.17f;
    p.settle = 0.05f;
    p.tau = 0.4f;
    return p;
}

inline float Wrap(float a) { return std::remainder(a, 6.28318531f); }

class Follow {
public:
    // A new F3 shot: take the face as it is.
    void Reset(float yaw, float pitch, const float eye[3]) {
        yaw_ = yaw;
        pitch_ = pitch;
        for (int k = 0; k < 3; ++k) eye_[k] = eye[k];
        lastTarget_ = yaw;
        turning_ = false;
        have_ = true;
    }
    // One frame: the face heading / elevation and the eye point the animation has now; usePitch = fp_f3_pitch.
    void Update(float targetYaw, float targetPitch, const float eye[3], float dt, bool usePitch, const Params& p) {
        if (!have_) {
            Reset(targetYaw, usePitch ? targetPitch : 0.f, eye);
            return;
        }
        const float jump = std::fabs(Wrap(targetYaw - lastTarget_));
        lastTarget_ = targetYaw;
        float d = Wrap(targetYaw - yaw_);
        if (jump > p.cut) {  // a camera cut: no slow orbit through it
            yaw_ = targetYaw;
            turning_ = false;
            d = 0.f;
        }
        if (std::fabs(d) > p.deadzone) turning_ = true;
        if (turning_) {
            yaw_ = Wrap(yaw_ + d * (1.f - std::exp(-dt / p.tau)));
            if (std::fabs(Wrap(targetYaw - yaw_)) < p.settle) turning_ = false;
        }
        const float wantPitch = usePitch ? targetPitch : 0.f;
        pitch_ += (wantPitch - pitch_) * (1.f - std::exp(-dt / p.pitchTau));
        float step = 0.f;
        for (int k = 0; k < 3; ++k) step += (eye[k] - eye_[k]) * (eye[k] - eye_[k]);
        const float a = p.posTau <= 0.f || std::sqrt(step) > p.posCut ? 1.f : 1.f - std::exp(-dt / p.posTau);
        for (int k = 0; k < 3; ++k) eye_[k] += (eye[k] - eye_[k]) * a;
    }
    float Yaw() const { return yaw_; }
    float Pitch() const { return pitch_; }
    const float* Eye() const { return eye_; }
    bool Turning() const { return turning_; }

private:
    float yaw_ = 0.f, pitch_ = 0.f, lastTarget_ = 0.f, eye_[3] = {0.f, 0.f, 0.f};
    bool turning_ = false, have_ = false;
};

}  // namespace hrvr::f3
