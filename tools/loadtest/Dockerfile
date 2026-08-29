# Build stage
FROM golang:1.24-alpine AS builder

WORKDIR /src

# Install certificates and git
RUN apk add --no-cache ca-certificates git

# Cache dependencies
COPY go.mod go.sum ./
RUN go mod download

# Copy source code
COPY . .

# Build statically linked binary
RUN CGO_ENABLED=0 GOOS=linux go build -ldflags="-s -w" -o /bin/loadtest .

# Runtime stage
FROM alpine:3.20

RUN apk add --no-cache ca-certificates tzdata

COPY --from=builder /bin/loadtest /usr/local/bin/loadtest

ENTRYPOINT ["/usr/local/bin/loadtest"]
CMD ["--help"]
