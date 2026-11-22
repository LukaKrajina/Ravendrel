#pragma once
#include <vector>
#include <string>
#include <utility>
#include <cmath>
#include <iostream>
#include "hardware/observability.hpp"
#include "qpc/math.hpp"
#include "qpc/joint.hpp"
#include "../qcdrc/camera.hpp"
#include "consciousness_controller.hpp"
#include "rl_agent.hpp"
#include "rl_pipeline.hpp"

namespace rvdlrsp::control
{


    struct ProstheticJoint
    {
        std::string name;
        qpc::JointType type = qpc::JointType::Hinge;
        double min_limit = -1.2;
        double max_limit = +1.2;
        double rest_angle = 0.0;
        qpc::Vec3 axis{1.0, 0.0, 0.0};
    };

    struct ProstheticLimbConfig
    {
        double intent_gain = 1.0;
        double assist_gain = 0.3;
        double lr = 0.01;
    };

    struct BionicEyeConfig
    {
        double pan_min = -1.5, pan_max = 1.5;
        double tilt_min = -1.0, tilt_max = 1.0;
        double assist_gain = 0.3;
        double lr = 0.01;
    };

    class ProstheticLimb
    {
    private:
        std::vector<ProstheticJoint> joints_;
        std::vector<double> angles_;
        std::vector<double> target_angles_;
        ProstheticLimbConfig cfg_;
        std::vector<QuantumRLAgent> agents_;
        std::vector<std::vector<double>> last_obs_;

    public:
        explicit ProstheticLimb(std::vector<ProstheticJoint> joints,
                                ProstheticLimbConfig cfg = ProstheticLimbConfig{})
            : joints_(std::move(joints)), cfg_(cfg)
        {
            angles_.assign(joints_.size(), 0.0);
            target_angles_.assign(joints_.size(), 0.0);
            for (size_t i = 0; i < joints_.size(); ++i)
            {
                agents_.emplace_back(2, 1, cfg_.lr);
                last_obs_.emplace_back(2, 0.0);
            }
            RVDLRSP_INFO("prosthetic") << "Prosthetic limb online ("
                                        << joints_.size() << " joints).";
        }

        size_t joint_count() const { return joints_.size(); }
        const std::vector<ProstheticJoint> &joints() const { return joints_; }
        const std::vector<double> &angles() const { return angles_; }
        const std::vector<double> &target_angles() const { return target_angles_; }

        void set_backend(qhal::IQuantumBackend *be)
        {
            for (auto &a : agents_)
                a.set_backend(be);
        }

        void set_intent(const std::vector<int> &brain_bits)
        {
            ConsciousnessModulators m = ConsciousnessController::compute(brain_bits);
            double neutral = m.arousal - 0.5;
            for (size_t i = 0; i < joints_.size(); ++i)
            {
                double amp = (joints_[i].max_limit - joints_[i].min_limit) * 0.5;
                double target = joints_[i].rest_angle + neutral * amp * 2.0 * cfg_.intent_gain;
                target_angles_[i] = std::max(joints_[i].min_limit,
                                             std::min(joints_[i].max_limit, target));
            }
        }

        void set_target(const std::vector<double> &target)
        {
            for (size_t i = 0; i < joints_.size() && i < target.size(); ++i)
                target_angles_[i] = std::max(joints_[i].min_limit,
                                             std::min(joints_[i].max_limit, target[i]));
        }

        std::vector<double> step()
        {
            std::vector<double> assists(joints_.size(), 0.0);
            for (size_t i = 0; i < joints_.size(); ++i)
            {
                double intent_offset = target_angles_[i] - joints_[i].rest_angle;
                std::vector<double> obs{angles_[i], intent_offset};
                std::vector<double> assist = agents_[i].act_quantum(obs);
                assists[i] = assist.empty() ? 0.0 : assist[0];
                double cmd = target_angles_[i] + assists[i] * cfg_.assist_gain;
                cmd = std::max(joints_[i].min_limit, std::min(joints_[i].max_limit, cmd));
                angles_[i] += (cmd - angles_[i]) * 0.2;
                last_obs_[i] = obs;
            }
            return assists;
        }

        void feedback(double reward)
        {
            for (size_t i = 0; i < joints_.size(); ++i)
            {
                double err = target_angles_[i] - angles_[i];
                double assist = (target_angles_[i] >= angles_[i]) ? 0.1 : -0.1;
                agents_[i].store(last_obs_[i], {assist}, reward - err * err);
            }
        }

        void train(int epochs = 1)
        {
            for (auto &a : agents_)
                a.train(epochs);
        }
    };

    class BionicEye
    {
    private:
        BionicEyeConfig cfg_;
        double pan_ = 0.0, tilt_ = 0.0;
        double target_pan_ = 0.0, target_tilt_ = 0.0;
        QuantumRLAgent pan_agent_;
        QuantumRLAgent tilt_agent_;
        std::vector<double> last_pan_obs_{2, 0.0};
        std::vector<double> last_tilt_obs_{2, 0.0};

    public:
        explicit BionicEye(BionicEyeConfig cfg = BionicEyeConfig{})
            : cfg_(cfg), pan_agent_(2, 1, cfg.lr), tilt_agent_(2, 1, cfg.lr)
        {
            RVDLRSP_INFO("prosthetic") << "Bionic eye online.";
        }

        void set_backend(qhal::IQuantumBackend *be)
        {
            pan_agent_.set_backend(be);
            tilt_agent_.set_backend(be);
        }

        void set_intent(const std::vector<int> &brain_bits)
        {
            ConsciousnessModulators m = ConsciousnessController::compute(brain_bits);
            target_pan_ = (m.arousal - 0.5) * (cfg_.pan_max - cfg_.pan_min);
            target_tilt_ = (m.arousal - 0.5) * (cfg_.tilt_max - cfg_.tilt_min) * 0.5;
        }

        void set_target(double pan, double tilt)
        {
            target_pan_ = std::max(cfg_.pan_min, std::min(cfg_.pan_max, pan));
            target_tilt_ = std::max(cfg_.tilt_min, std::min(cfg_.tilt_max, tilt));
        }

        void step()
        {
            std::vector<double> pan_obs{pan_, target_pan_};
            std::vector<double> tilt_obs{tilt_, target_tilt_};
            auto pa = pan_agent_.act_quantum(pan_obs);
            auto ta = tilt_agent_.act_quantum(tilt_obs);
            double pan_assist = pa.empty() ? 0.0 : pa[0];
            double tilt_assist = ta.empty() ? 0.0 : ta[0];

            double pan_cmd = target_pan_ + pan_assist * cfg_.assist_gain;
            double tilt_cmd = target_tilt_ + tilt_assist * cfg_.assist_gain;
            pan_ += (std::max(cfg_.pan_min, std::min(cfg_.pan_max, pan_cmd)) - pan_) * 0.3;
            tilt_ += (std::max(cfg_.tilt_min, std::min(cfg_.tilt_max, tilt_cmd)) - tilt_) * 0.3;

            last_pan_obs_ = pan_obs;
            last_tilt_obs_ = tilt_obs;
        }

        void feedback(double reward)
        {
            double pan_err = target_pan_ - pan_;
            double tilt_err = target_tilt_ - tilt_;
            double pan_assist = (target_pan_ >= pan_) ? 0.1 : -0.1;
            double tilt_assist = (target_tilt_ >= tilt_) ? 0.1 : -0.1;
            pan_agent_.store(last_pan_obs_, {pan_assist}, reward - pan_err * pan_err);
            tilt_agent_.store(last_tilt_obs_, {tilt_assist}, reward - tilt_err * tilt_err);
        }

        void train(int epochs = 1)
        {
            pan_agent_.train(epochs);
            tilt_agent_.train(epochs);
        }

        qpc::Vec3 gaze_direction() const
        {
            return {
                -std::cos(tilt_) * std::sin(pan_),
                std::sin(tilt_),
                -std::cos(tilt_) * std::cos(pan_)};
        }

        double pan() const { return pan_; }
        double tilt() const { return tilt_; }
        double target_pan() const { return target_pan_; }
        double target_tilt() const { return target_tilt_; }
    };

    class ProstheticEnvironment : public IEnvironment
    {
    private:
        ProstheticLimb limb_;
        std::vector<double> target_;
        int step_ = 0;

    public:
        explicit ProstheticEnvironment(std::vector<ProstheticJoint> joints,
                                       ProstheticLimbConfig cfg = ProstheticLimbConfig{})
            : limb_(std::move(joints), cfg)
        {
            target_.assign(limb_.joint_count(), 0.0);
        }

        std::vector<double> reset() override
        {
            step_ = 0;
            for (size_t i = 0; i < target_.size(); ++i)
                target_[i] = limb_.joints()[i].rest_angle;
            limb_.set_target(target_);
            return observe();
        }

        StepResult step(const std::vector<double> &action) override
        {
            if (action.size() == target_.size())
                limb_.set_target(action);

            limb_.step();
            ++step_;

            double reward = 0.0;
            const auto &a = limb_.angles();
            for (size_t i = 0; i < a.size(); ++i)
            {
                double err = target_[i] - a[i];
                reward -= err * err;
            }

            StepResult r;
            r.observation = observe();
            r.reward = reward;
            r.done = (step_ >= 100);
            return r;
        }

        size_t observation_dim() const override { return limb_.joint_count() * 2; }
        size_t action_dim() const override { return limb_.joint_count(); }

        ProstheticLimb &limb() { return limb_; }

    private:
        std::vector<double> observe()
        {
            std::vector<double> obs;
            obs.reserve(limb_.joint_count() * 2);
            const auto &a = limb_.angles();
            const auto &t = limb_.target_angles();
            for (size_t i = 0; i < limb_.joint_count(); ++i)
            {
                obs.push_back(a[i]);
                obs.push_back(t[i]);
            }
            return obs;
        }
    };

    inline std::vector<ProstheticJoint> make_prosthetic_arm_joints()
    {
        return {
            {"shoulder", qpc::JointType::BallSocket, -1.5, 1.5, 0.0, {1.0, 0.0, 0.0}},
            {"elbow", qpc::JointType::Hinge, -2.0, 0.2, 0.0, {1.0, 0.0, 0.0}},
            {"wrist", qpc::JointType::BallSocket, -1.0, 1.0, 0.0, {0.0, 0.0, 1.0}},
            {"finger_thumb", qpc::JointType::Hinge, 0.0, 1.0, 0.0, {1.0, 0.0, 0.0}},
            {"finger_index", qpc::JointType::Hinge, 0.0, 1.2, 0.0, {1.0, 0.0, 0.0}},
        };
    }

    class BionicEyeCamera : public qcdrc::ICamera
    {
    private:
        BionicEye eye_;
        qpc::Vec3 target_world_{0.0, 0.0, -3.0};
        qcdrc::CameraIntrinsics intrinsics_{600.0, 600.0, 320.0, 240.0};
        int width_ = 640;
        int height_ = 480;
        uint64_t frame_counter_ = 0;

    public:
        explicit BionicEyeCamera(BionicEyeConfig cfg = BionicEyeConfig{}) : eye_(cfg)
        {
            RVDLRSP_INFO("prosthetic") << "Bionic eye camera online.";
        }

        BionicEye &eye() { return eye_; }

        void set_target_world(const qpc::Vec3 &t) { target_world_ = t; }

        qcdrc::RgbFrame capture() override
        {
            eye_.step();
            qpc::Vec3 gaze = eye_.gaze_direction();

            qpc::Vec3 up{0.0, 1.0, 0.0};
            qpc::Vec3 right = up.cross(gaze).normalized();
            if (right.length_sq() < 1e-12)
                right = {1.0, 0.0, 0.0};
            qpc::Vec3 up_cam = gaze.cross(right).normalized();

            double tx = target_world_.dot(right);
            double ty = target_world_.dot(up_cam);
            double tz = target_world_.dot(gaze);

            bool visible = tz > 0.1;
            int u = -1, v = -1;
            if (visible)
            {
                u = static_cast<int>(intrinsics_.fx * tx / tz + intrinsics_.cx);
                v = static_cast<int>(intrinsics_.fy * ty / tz + intrinsics_.cy);
            }

            qcdrc::RgbFrame frame;
            frame.width = width_;
            frame.height = height_;
            frame.pixels.assign(static_cast<size_t>(width_) * height_ * 3, 24);
            frame.timestamp_us = frame_counter_++ * 16666;

            if (visible && u >= 0 && u < width_ && v >= 0 && v < height_)
            {
                const int r = 20;
                for (int dy = -r; dy <= r; ++dy)
                {
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        if (dx * dx + dy * dy > r * r)
                            continue;
                        int px = u + dx, py = v + dy;
                        if (px < 0 || px >= width_ || py < 0 || py >= height_)
                            continue;
                        size_t idx = (static_cast<size_t>(py) * width_ + px) * 3;
                        frame.pixels[idx] = 255;
                        frame.pixels[idx + 1] = 200;
                        frame.pixels[idx + 2] = 0;
                    }
                }
            }
            return frame;
        }

        bool is_open() const override { return true; }
        std::string name() const override { return "BionicEyeCamera"; }
    };
}