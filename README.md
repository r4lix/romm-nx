# romm-nx

**romm-nx** is an unofficial Nintendo Switch homebrew client for [RomM](https://github.com/rommapp/romm).

It allows you to browse your RomM library and download games from your own RomM server directly to your Nintendo Switch.

> [!IMPORTANT]
> romm-nx is a personal project intended as a temporary solution until a more official or robust alternative becomes available.
>
> It is **not affiliated with, maintained by, or officially supported by the RomM team**.

## Disclaimer

This project was created **entirely with the help of AI coding tools**.

I am **not a developer**, so the code may contain bugs, security vulnerabilities, mistakes, or unfinished features. romm-nx is a personal project that I decided to share with others who may find it useful.

Use it at your own risk. I cannot guarantee support, stability, compatibility, security, or regular updates.

## Current features

* Connect to a self-hosted RomM server
* Browse platforms and games
* Display game covers and details
* Download games directly to the Nintendo Switch
* Manage downloaded games
* Configure download paths for supported platforms

Some features are still experimental or incomplete.

## To-do

* File browser improvements — currently in beta
* Save management
* Cheats and mods management
* Game launcher

## Installation

1. Download the latest `romm-nx.nro` release.
2. Copy it to:

```text
sdmc:/switch/romm-nx/romm-nx.nro
```

3. Launch romm-nx from the Homebrew Menu.
4. Configure the address and credentials of your RomM server.

Optionally, after launching the application for the first time, a configuration file will be created at:

```text
sdmc:/switch/romm-nx/config.json
```

You can paste your RomM user API key into this file instead of entering it manually using the Nintendo Switch keyboard.

A self-hosted RomM server is required.

## Legal notice

romm-nx does not provide, host, or distribute games.

You are responsible for the content stored on your RomM server and for complying with the laws applicable in your country.

## Issues and contributions

Bug reports and feedback are welcome.

Because I am not a developer, I may not be able to fix every issue or provide technical support. Pull requests and contributions from experienced developers are greatly appreciated.

When reporting an issue, please include as much information as possible, such as the romm-nx version, your RomM version, the affected platform, and any available logs.

## Thanks and credits

* [RomM](https://github.com/rommapp/romm) for the original self-hosted ROM manager
* The Nintendo Switch homebrew community
* The open-source projects and libraries used by romm-nx
* The AI coding tools used to help build the project: Antigravity, Claude Code, and ChatGPT
