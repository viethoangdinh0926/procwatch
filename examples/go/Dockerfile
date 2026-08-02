# CGO_ENABLED=0 produces a fully static binary, which is the common case for
# Go in containers and exactly the case LD_PRELOAD cannot reach. That is the
# point of this sample: it demonstrates the documented limitation rather than
# working around it.
FROM golang:1.23-alpine AS build
WORKDIR /src
COPY go/main.go .
RUN go mod init inventory >/dev/null 2>&1 || true
RUN CGO_ENABLED=0 go build -o /out/inventory main.go

FROM alpine:3.20
COPY --from=build /out/inventory /usr/local/bin/inventory
CMD ["/usr/local/bin/inventory"]
