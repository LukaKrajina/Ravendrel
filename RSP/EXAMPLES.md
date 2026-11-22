# RSP — Robotics/Simulation/Physics Examples

This directory contains **example applications** demonstrating the rvdlRSP platform capabilities.

## Quick Start

```bash
# Configure with examples enabled
cmake -B build -DRVDLRSP_BUILD_EXAMPLES=ON ..
cmake --build build --target rvdlRSP_rl_physics_demo
./build/rvdlRSP_rl_physics_demo
```

---

## Examples

### `rvdlRSP_rl_physics_demo` — RL Training on Physics Environment

**File:** `examples/rl_physics_demo.cpp`

End-to-end reinforcement learning demonstration using the **Quantum RL Agent** on a real physics environment (QVM backend).

**What it does:**
1. Creates a `PhysicsEnvironment` (wraps QVM quantum backend)
2. Instantiates a `QuantumRLAgent` with observation/action dimensions from the environment
3. Injects the environment's quantum backend into the agent for quantum estimation
4. Runs `RLPipeline::train()` for 50 episodes × 120 steps each
5. Reports total accumulated reward

**Configuration (in code):**
```cpp
RLConfig cfg;
cfg.episodes = 50;
cfg.max_steps = 120;
cfg.learning_rate = 0.02;
cfg.train_every = 5;
```

**Requirements:**
- Quantum backend (QVM) must be available — runs on the same machine as the runtime daemon
- Daemon must be running: `./runtime --daemon` (or systemd service `ravendrel-runtime.service`)

**Output:**
```
[rvdlRSP.rl] End-to-end RL training on physics environment...
[rvdlRSP.rl] Training complete. Total reward = <value>
```

---

## Adding New Examples

1. Create `examples/<name>.cpp` with `int main()`
2. Add target in `CMakeLists.txt` under `RVDLRSP_BUILD_EXAMPLES`
3. Link against `rvdl_rt` and `Kokkos::kokkos`
4. Document here with purpose, configuration, and requirements

---

## Related

- **Benchmarks:** `../runtime/BENCHMARKS.md` — Physics/math validation (separate from examples)
- **Tests:** `tests/` — Unit/integration test suite (run via `ctest`)
- **Qt GUI:** `gui/qt/` — Full simulation platform UI (`rvdlRSP_gui` target)