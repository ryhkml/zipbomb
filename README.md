# Zip Bomb

Serve a GZIP bomb as a defense mechanism against malicious requests.

## Overview

Zip Bomb creates a server that responds to requests with a pre-generated GZIP bomb file (data.gzip).
It is designed to target attackers or scanners by sending a large decompressed file (~10GB) hidden in a small compressed form (~10MB).

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
    --host [addr]        Set Zip Bomb host (default: 127.0.0.1)
    --port [4000-65535]  Set Zip Bomb port (default: 32000)
```

Use nginx as a reverse proxy. An example configuration is in [nginx.conf](./nginx.conf).

## Related Article

You can also read related article on [Zip Bombs](https://blog.haschek.at/2017/how-to-defend-your-website-with-zip-bombs.html).
