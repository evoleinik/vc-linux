# notanemulator BBS

ENiGMA½ 0.5.1-beta (BSD-2), running on the `axis` k3s node. VC dials it with MS-DOS Kermit
through a virtual modem. Design: `docs/plans/2026-10-01-bbs-experience.md`.

| What | Where |
|---|---|
| Public WebSocket | `wss://axis.tail85247.ts.net:8443/`, by Tailscale Funnel to node port 30810 |
| Telnet | node port 30888, inside the tailnet only |
| Data | PVC `bbs/enigma-data` (config, menus, art, db, filebase, logs) |
| Sysop | handle `eugene`, user 1, groups users and sysops; password in `~/.config/vc-linux/bbs-sysop.txt` on box |

The image is pinned by digest in `enigma.yaml`. Funnel on axis also serves n8n on 443; leave it.

## First-time setup, as done on 2026-10-01

1. `kubectl apply -f enigma.yaml`, then scale `deploy/enigma` to 0.
2. `kubectl apply -f setup-pod.yaml`. In the pod, copy `/enigma-bbs-pre/{config,mods,art}` into the
   empty volume folders, then run `./oputil.js config new` (board name `notanemulator BBS`,
   defaults elsewhere). It is interactive, so drive it through a pseudo-terminal.
3. In `config/config.hjson`, set `loginServers.webSocket.proxied: true` and `ws.enabled: true`.
4. Delete the setup pod and scale `deploy/enigma` to 1.
5. Register the sysop before exposing anything: the first account to apply becomes user 1, the
   sysop. `sysop`, `admin` and `root` are reserved names.
6. `tailscale funnel --bg --https=8443 http://127.0.0.1:30810`.

## Day to day

    sudo k3s kubectl -n bbs logs deploy/enigma --tail=50
    sudo k3s kubectl -n bbs exec deploy/enigma -- ./oputil.js user info HANDLE
    sudo k3s kubectl -n bbs exec deploy/enigma -- ./oputil.js user deactivate HANDLE
    sudo tailscale funnel --https=8443 off        # take the BBS offline
