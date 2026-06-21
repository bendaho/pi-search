# PI SEARCH

Search for your number in 1,000,000,000 digits of Pi.

## Features

- **Fast search**: C++ backend with `std::string::find` (SSE2-optimized)
- **SQLite cache**: repeated queries return instantly (~0.06ms)
- **PDF certificates**: download a stylish certificate showing where your number appears in Pi
- **Geeky design**: terminal-style dark UI with Matrix rain background

## Requirements

- g++ (C++17)
- ~1GB RAM for loading pi digits

## Setup

```bash
# Download pi digits (1 billion)
curl -L -o pi-billion.txt https://stuff.mit.edu/afs/sipb/contrib/pi/pi-billion.txt

# Compile
g++ -O3 -c sqlite3.c -o sqlite3.o
g++ -O3 -c server.cpp -o server.o -std=c++17
g++ -O3 -o server server.o sqlite3.o -lpthread -ldl

# Run
./server 8080
```

Open `http://localhost:8080`

## API

| Endpoint | Method | Description |
|---|---|---|
| `/api/search?q=NUMBER` | GET | Search pi for a digit sequence |
| `/api/certificate?q=NUMBER` | GET | Download PDF certificate |
| `/api/stats` | GET | Server status |

## Tech Stack

- **Backend**: C++17, raw POSIX sockets, SQLite (amalgamation)
- **Frontend**: Vanilla HTML/CSS/JS, JetBrains Mono font
- **Data**: [MIT SIPB pi-billion.txt](https://stuff.mit.edu/afs/sipb/contrib/pi/)
