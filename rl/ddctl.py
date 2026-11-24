"""낙폭 제어 오버레이(Grossman-Zhou식 drawdown control).
바닥 = (1 - limit) * 고점. 주식 노출 상한 = mult * (자산 - 바닥) / 자산 (0~1로 클리핑).
하루 주식 손실률이 1/mult 미만이면 고점 대비 낙폭이 limit을 넘지 않음(비용 제외).
대가: 바닥 근처에 닿으면 노출이 0 가까이 줄어 회복이 매우 느려질 수 있음(현금 고착)."""
import numpy as np


def exposure_cap(value, peak, limit, mult):
    floor = (1.0 - limit) * peak
    return float(np.clip(mult * (value - floor) / value, 0.0, 1.0))


def apply(w, value, peak, limit, mult):
    """w: [N+1] 목표비중(마지막 현금). 주식 합이 상한을 넘으면 비례 축소하고 나머지는 현금."""
    if limit <= 0:
        return w
    cap = exposure_cap(value, peak, limit, mult)
    s = float(w[:-1].sum())
    if s <= cap or s <= 0:
        return w
    out = w.copy()
    out[:-1] *= cap / s
    out[-1] = 1.0 - out[:-1].sum()
    return out
