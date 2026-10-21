# 3.11: capstone 5.0.1 still imports distutils, removed in 3.12.
FROM python:3.11-slim
RUN pip install --no-cache-dir capstone==5.0.1 pillow==10.4.0
WORKDIR /work
# -E -s: ignore PYTHON* env vars and user site-packages; sys.path[0] is /tools (read-only mount).
ENTRYPOINT ["python", "-E", "-s"]
