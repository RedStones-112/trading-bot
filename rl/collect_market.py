"""시장 상태 특성용 SPY/VIX 일봉 -> rl/data/market_long.parquet"""
import yfinance as yf, pandas as pd
from datetime import date, timedelta
s = str(date.today() - timedelta(days=365 * 22)); e = str(date.today())
spy = yf.download("SPY", start=s, end=e, auto_adjust=True, progress=False)["Close"].squeeze()
vix = yf.download("^VIX", start=s, end=e, progress=False)["Close"].squeeze()
pd.DataFrame({"spy": spy, "vix": vix}).dropna().to_parquet("rl/data/market_long.parquet")
print("ok", len(spy), len(vix))
