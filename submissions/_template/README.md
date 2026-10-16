Language: your-language-here

# my-fizzbuzz-server

I starred fizzbuzz-arena

One paragraph about your approach. What did you optimize and why?

## Files

- `install.sh` (optional): runs once with sudo on a clean Ubuntu 24.04 runner.
  Install your toolchain here, pin versions.
- `build.sh` (optional): builds your server as a normal user.
- `start.sh` (required): starts your server in the FOREGROUND. It must read
  the port from `$PORT` (default 80).
- your code

## Rules checklist

- [ ] `README.md` contains the line `I starred fizzbuzz-arena`
- [ ] `start.sh` runs the server in the foreground and reads `$PORT`
- [ ] `GET /N` answers 200 with exact bytes: `fizz`, `buzz`, `fizzbuzz` or the digits
- [ ] junk paths answer 4xx, non-GET methods answer 4xx
- [ ] HTTP/1.1 keep-alive works, every response has Content-Length
- [ ] no hardcoding the test sequence, no load-shedding
