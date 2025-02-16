# Pipewire Linking API
Minimal REST API for querying and linking ports in Pipewire

## Usage

Listens on 127.0.0.1:9080

### /ports
GET: Return two lists of source (output/capture) and destination (input/playback) ports, respectively

```json
[
    [
        "node1:source_port1",
        "node1:source_port1",
        "node2:source_port1",
        "node2:source_port2",
    ],
    [
        "node1:dest_port1",
        "node1:dest_port1",
        "node3:dest_port1",
        "node3:dest_port2",
    ]
]
```

### /links/active
GET: Return list of existing active links as source/destination pairs 
```json
[
    ["node1:source_port1", "node1:dest_port1"],
    ["node1:source_port2", "node1:dest_port2"]
]
```

### /links/desired
GET: Return list of desired links, whether active or not
```json
[
    ["node1:source_port1", "node1:dest_port1"],
    ["node1:source_port1", "node1:dest_port2"],
    ["node4:source_port1", "node3:dest_port1"],
    ["node4:source_port2", "node3:dest_port2"]
]
```
PUT: Set the list of desired links, linking and unlinking any active referenced ports as required. Input data matches format of GET.
PATCH: Add desired links to existing list, linking any additional ports as required. Existing links remain unchanged (list is de-duplicated)

### /links/remove
POST: Remove specified active and desired links if present. Input data format matches format of /links/desired

## ToDo

* More than 1 source file :)
* Command line options for log level, binding, clean-up behaviour on exit
* Detailed port info
* Debian packaging
