FROM alpine

WORKDIR /service/stonky

RUN apk --no-cache add sqlite sqlite-dev build-base libcurl libpq openssl gcc make g++ zlib-dev alpine-sdk curl curl-dev

ADD . /service/stonky

RUN make

CMD ["./stonky"]

