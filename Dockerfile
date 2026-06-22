FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y g++ curl unzip && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY server.cpp sqlite3.c sqlite3.h ./

RUN gcc -O3 -c sqlite3.c -o sqlite3.o -lpthread && \
    g++ -O3 -c server.cpp -o server.o -std=c++17 && \
    g++ -O3 -o server server.o sqlite3.o -lpthread -ldl

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y curl && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - && \
    apt-get install -y nodejs && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/server ./
COPY static/ ./static/
COPY sert2.html ./
COPY html2pdf.mjs ./
COPY package.json package-lock.json ./
RUN npm ci --omit=dev

RUN curl -L -o pi-billion.txt https://stuff.mit.edu/afs/sipb/contrib/pi/pi-billion.txt

EXPOSE 8080
CMD ["./server", "8080"]
