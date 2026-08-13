# Build Stage
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy source code
COPY . .

# Configure and build
RUN cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build-release --parallel $(nproc)

# Runtime Stage
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libpq5 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy only the compiled binaries we need
COPY --from=builder /app/build-release/src/tools/mdp-health /usr/local/bin/mdp-health
# Assuming the main processor executable is built, we would copy it here. 
# Based on the structure, we'll make sure health check is available.
# In a real scenario, the main service would be executed.
# COPY --from=builder /app/build-release/src/market_data_processor /usr/local/bin/market_data_processor

# Set default command
CMD ["mdp-health"]
