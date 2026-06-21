# Zip Bomb

Serve a GZIP bomb as a defense mechanism against malicious requests.

## Overview

Zip Bomb creates a server that responds to requests with a pre-generated GZIP bomb file (data.gzip).
It is designed to target attackers or scanners by sending a large decompressed file (~10GB) hidden in a small compressed form (~10MB).

The server only sends the bomb to clients that advertise `Accept-Encoding: gzip` — those are the ones that will decompress it and choke. Any other client (e.g. plain `curl` with no `Accept-Encoding`) gets a plain `404` instead, so no bandwidth is wasted on a target that can't inflate the payload.

## Background

The inspiration stems from the common issue of web server exposure to malicious scans, as highlighted in a Mastodon post by [@Viss](https://mastodon.social/@Viss/114864117312657608) on July 17, 2025, at 2:23 AM.
The post describes how putting a web server online anywhere invites **the background radiation of the internet**, as shown in the attached server log screenshot.
The log reveals numerous 404 responses to requests for sensitive files (e.g., config.yml, secrets.yml), indicating automated scanning by attackers.
Zip Bomb aims to counter such threats using a GZIP bomb defense mechanism.

## Prerequisites

A pre-generated data.gzip file

```sh
truncate -s 10G data.tmp
gzip < data.tmp > data.gzip
rm -rf data.tmp
```

Build

```sh
gcc -O2 -Wall -Wextra -Wformat -Wformat-security -fstack-protector-strong -std=c17 -o zipbomb main.c
```

## Usage

```
./zipbomb [OPTIONS]

Options:
    --host [addr]              Set Zip Bomb host (default: 127.0.0.1)
    --port [4000-65535]        Set Zip Bomb port (default: 32000)
    --file [path]              Set GZIP bomb file path (default: data.gzip)
    --workers [1-128]          Set worker process count (default: 4)
    --send-timeout [1-300]     Set per-client send timeout in seconds (default: 5)
```

Use nginx as a reverse proxy. An example configuration is in [nginx.conf](./nginx.conf).

> [!CAUTION]
> Do not enable `gunzip on;` on the proxy. The upstream serves a pre-gzipped body that inflates to ~10GB; `gunzip` would decompress it on your own nginx server and OOM you instead of the scanner.

> [!NOTE]
> The server gates on `Accept-Encoding: gzip`, so the proxy must forward that header. nginx forwards it to the upstream by default — do not clear it with `proxy_set_header Accept-Encoding "";` or every request will get a `404` instead of the bomb.

## Related Article

You can also read related article on [Zip Bombs](https://blog.haschek.at/2017/how-to-defend-your-website-with-zip-bombs.html).
