FROM python:3.9-slim

WORKDIR /app
RUN apt-get update
RUN apt-get install libibverbs1 libibverbs-dev librdmacm1 librdmacm-dev rdmacm-utils ibverbs-utils rdma-core -y

COPY libs/ libs/
COPY initialize.py .
COPY cmd.py .


