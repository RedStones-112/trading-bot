"""NYSE 거래일 판정(규칙 기반, tz 데이터베이스 불필요 -- 안드로이드/파이 공통).
휴장일 규칙: 신정(토요일이면 대체 없음, 일요일이면 월), MLK(1월 셋째 월), 대통령의날(2월 셋째 월), 성금요일,
메모리얼(5월 마지막 월), 준틴스(6/19, 2022~), 독립기념일(7/4), 노동절(9월 첫 월), 추수감사절(11월 넷째 목),
성탄절(12/25). 토요일 휴일은 금요일, 일요일 휴일은 월요일 대체. 조기폐장/임시휴장(국상 등)은 반영 안 함."""
from datetime import date, datetime, timedelta, timezone


def _nth_weekday(y, m, wd, n):
    d = date(y, m, 1)
    d += timedelta(days=(wd - d.weekday()) % 7)
    return d + timedelta(weeks=n - 1)


def _last_weekday(y, m, wd):
    d = date(y, m + 1, 1) - timedelta(days=1) if m < 12 else date(y, 12, 31)
    return d - timedelta(days=(d.weekday() - wd) % 7)


def _easter(y):  # 그레고리력 부활절(Anonymous Gregorian algorithm)
    a, b, c = y % 19, y // 100, y % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25; g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    month = (h + l - 7 * m + 114) // 31
    return date(y, month, (h + l - 7 * m + 114) % 31 + 1)


def _observed(d, saturday_shift=True):
    if d.weekday() == 5:
        return d - timedelta(days=1) if saturday_shift else None
    if d.weekday() == 6:
        return d + timedelta(days=1)
    return d


def holidays(y):
    hs = [_observed(date(y, 1, 1), saturday_shift=False),
          _nth_weekday(y, 1, 0, 3), _nth_weekday(y, 2, 0, 3), _easter(y) - timedelta(days=2),
          _last_weekday(y, 5, 0), _observed(date(y, 7, 4)), _nth_weekday(y, 9, 0, 1),
          _nth_weekday(y, 11, 3, 4), _observed(date(y, 12, 25))]
    if y >= 2022:
        hs.append(_observed(date(y, 6, 19)))
    return {h for h in hs if h}


def is_trading_day(d):
    return d.weekday() < 5 and d not in holidays(d.year)


def prev_trading_day(d):
    d -= timedelta(days=1)
    while not is_trading_day(d):
        d -= timedelta(days=1)
    return d


def now_et(now_utc=None):
    """미국 동부시간(서머타임: 3월 둘째 일요일 02:00 ~ 11월 첫째 일요일 02:00)."""
    u = now_utc or datetime.now(timezone.utc)
    y = u.year
    start = datetime.combine(_nth_weekday(y, 3, 6, 2), datetime.min.time(), timezone.utc) + timedelta(hours=7)
    end = datetime.combine(_nth_weekday(y, 11, 6, 1), datetime.min.time(), timezone.utc) + timedelta(hours=6)
    return (u - timedelta(hours=4 if start <= u < end else 5)).replace(tzinfo=None)


def session_status(now_utc=None):
    """(오늘 ET 날짜, 정규장 개장 여부, 마지막으로 '완성된' 종가 날짜)."""
    et = now_et(now_utc)
    d = et.date()
    t = et.hour * 60 + et.minute
    open_today = is_trading_day(d)
    in_session = open_today and 9 * 60 + 30 <= t < 16 * 60
    last_complete = d if (open_today and t >= 16 * 60 + 15) else prev_trading_day(d)
    return d, in_session, last_complete
