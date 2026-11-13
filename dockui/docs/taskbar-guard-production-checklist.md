# Production Taskbar Guard — remaining implementation checklist

V0.11 is a simulation. Both default and explicit taskbar hiding remain disabled. This document authorizes no real taskbar test.

- [ ] Design authenticated user/session-specific ownership and a durable transaction identity that cannot be confused across simultaneous or stale Managers, guards, Explorer sessions or reboots; retain one independently enforced cross-process owner.
- [ ] Define and prove a write-ahead transaction for each actual window, including the ambiguity between a persisted Pending intent and a successfully applied hide. Commit every action durably, guard all visibility changes and preserve uncertain/conflicting records rather than guessing.
- [ ] Define safe independent recovery when Manager and Guard fail together, including Pending journal records; the current manual restore tool conservatively refuses Pending and cannot resolve that case automatically.
- [ ] Start only after Dock first successful Present, establish an authenticated guard-ready handshake, monitor Manager/Dock/guard exits and handle every startup/timeout/failure path without leaving taskbars hidden.
- [ ] Track primary and secondary taskbars with PID plus process-creation identity, inspect Explorer recreation, monitor plug/unplug, original visibility, and external user changes; avoid changing windows the session cannot prove it owns.
- [ ] Document and verify an independent manual recovery path, retained evidence on failures, conflict UX and idempotent recovery. Keep auto-hide registry/user settings and Explorer untouched.
- [ ] Add deterministic fault injection at every persistence/action boundary, double-failure, stale journal, lock loss, restore failure, duplicate launch and malformed records. Run isolated tests first.
- [ ] Only after independent review and explicit user confirmation, plan a separately authorized real Windows test with a prepared recovery method. Until then both default and explicit hide must remain fail-closed.
