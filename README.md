# contents-delivery-manager
Reimplementation of Nintendo Switch [contents-delivery](https://switchbrew.org/wiki/NIM_services#Contents_Delivery). This implements a server which [nssu-updater](https://github.com/switchbrew/nssu-updater) can connect to over the network for installing sysupdates (this is also used internally by [nssu-updater](https://github.com/switchbrew/nssu-updater) itself, see below).

### Usage
Output from running the app without args or with `-h`:

```
Usage: contents_delivery_manager [options]

--help,    -h   Display this information.
--log,     -l   Enable logging to the specified file, or if path not specified stdout.
--server,  -s   Run as a server (default).
--client,  -c   Run as a client.
--address, -a   Hostname or IPv4 address to bind/connect to. With server the default is 0.0.0.0.
--port,    -p   Port, the default is 55556.
--datadir, -d   Sysupdate data dir path.
--depth,   -e   Sysupdate data dir scanning depth, the default is 3.
--tmpdir,  -t   Temporary directory path used during datadir scanning, this will be automatically deleted when usage is finished. The default is 'tmpdir'.
```

Only server-mode should be used, client-mode is for testing. `--datadir` is required for server-mode. Example command: `./contents_delivery_manager --datadir {path} --log[=path]`

The datadir is the directory containing the sysupdate content data. The datadir is scanned recursively with a maximum depth of {see --depth option}. During scanning files/directories which have a name starting with '.' are ignored. Content filenames must be one of the following: `*{hex ContentId}`, `{hex ContentId}.nca`, or `{hex ContentId}.cnmt.nca`. Meta content must have filenames `ncatype0_*{hex ContentId}` (where `*` is ignored), or `{hex ContentId}.cnmt.nca`.

During Meta loading with datadir-scanning the Meta content is extracted using [hactool](https://github.com/SciresM/hactool) (see the `--tmpdir` option): you must have keys setup for decrypting [1.0.0+] content.

All content required for installing the sysupdate requested by the client must be present in the datadir: if the client is more than 1 system-version behind the requested version, make sure the content for those versions are also present if there isn't a newer version of that content.

Check the log when issues occur. For error-codes, see [switchbrew](https://switchbrew.org/wiki/Error_codes).

### Download
TODO

### Building
`./autogen.sh && mkdir build && cd build && ../configure && make`

The common/ directory can be used externally, [nssu-updater](https://github.com/switchbrew/nssu-updater) uses this.

#### Credits

* This uses the SHA256 implementation from [nettle](https://git.lysator.liu.se/nettle/nettle), bundled under common/nettle/. This is only used on pc, on Switch libnx is used.

