"""시드 병렬 학습 + 평가 요약. 사용: python -m rl.run_seeds --timesteps 3000000 --seeds 0 1 2 --prefix exp2
기존 CLI(rl.train / rl.evaluate)를 subprocess로만 호출. 로그: rl/models/{prefix}_s{seed}.log"""
import argparse, os, re, subprocess, sys, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timesteps", type=int, default=3000000)
    ap.add_argument("--seeds", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--prefix", default="exp2")
    ap.add_argument("--eval-only", action="store_true")
    a = ap.parse_args()
    env = dict(os.environ, OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", PYTHONIOENCODING="utf-8")
    paths = {s: f"rl/models/{a.prefix}_s{s}.zip" for s in a.seeds}
    t0 = time.time()
    if not a.eval_only:
        procs = []
        for s, p in paths.items():
            log = open(os.path.join(ROOT, f"rl/models/{a.prefix}_s{s}.log"), "w", encoding="utf-8")
            procs.append(subprocess.Popen(
                [sys.executable, "-m", "rl.train", "--timesteps", str(a.timesteps), "--seed", str(s), "--out", p],
                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT))
        for pr in procs:
            pr.wait()
        print(f"학습 소요 {(time.time() - t0) / 60:.1f}분", flush=True)
    rows = {}
    for s, p in paths.items():
        out = subprocess.run([sys.executable, "rl/evaluate.py", "--model", p], cwd=ROOT, env=env,
                             capture_output=True, text=True, encoding="utf-8").stdout
        print(f"--- seed {s}\n{out}")
        for line in out.splitlines():
            if line.startswith("PPO"):
                rows[("PPO", s)] = [float(x.strip("%")) for x in re.findall(r"-?\d+\.?\d*%?", line[3:])]
            elif line.startswith("동일비중 일일리밸"):
                rows[("EQ", s)] = [float(x.strip("%")) for x in re.findall(r"-?\d+\.?\d*%?", line[len("동일비중 일일리밸"):])]
    ppo = np.array([v for (k, _), v in rows.items() if k == "PPO"])
    if len(ppo):
        print("PPO 열: 누적%,연환산%,변동성%,샤프,MDD%,턴오버")
        print("mean", ppo.mean(0).round(3), "\nstd ", ppo.std(0, ddof=1).round(3) if len(ppo) > 1 else "")


if __name__ == "__main__":
    main()
