FROM alpine

RUN apk add python3 python3-dev gcc g++ musl-dev

ADD requirements.txt /tmp/requirements.txt

RUN pip3 install -r /tmp/requirements.txt

RUN mkdir -p /opt/proxy

WORKDIR /opt/proxy
