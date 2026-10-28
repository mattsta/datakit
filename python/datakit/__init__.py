"""datakit for Python.

``datakit.persist`` reads datakit's persisted files (snapshots, WALs and
quarantine records) for operators: inspect, verify, walk, recover and
read quarantined bytes, never changing a file.
"""

from . import persist

__all__ = ["persist"]
