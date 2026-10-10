# LlamaBoss PowerShell pending fix

Replace the nine matching files in the LlamaBoss project root, then rebuild
LlamaBoss in Visual Studio.

## What changed

- A PowerShell result can no longer wait forever because a descendant retained
  stdout or stderr pipe handles.
- Ordinary PowerShell execution is explicitly foreground-only. When the
  top-level PowerShell process exits, any remaining descendants in that job are
  terminated before the result is returned.
- Pipe draining has a two-second grace period and a forced synchronous-I/O
  cancellation fallback.
- A detached/background attempt returns a visible `background stopped` chip and
  a corrective explanation telling the model to use foreground execution or
  `Start-Process -Wait`.
- Pending async cards now show a live elapsed timer. PowerShell also shows its
  real hard timeout and `Stop cancels`. The temporary running card is replaced
  by the terminal result instead of remaining stale in the transcript.
- The agent prompt no longer permits detached or interactive PowerShell work.

## Focused verification after compiling

Run these in Agent mode with approvals left enabled.

### 1. Former permanent-pending reproduction

Ask the model to execute this exact PowerShell command:

```powershell
Start-Process -FilePath "$env:ComSpec" -ArgumentList '/c','ping -n 31 127.0.0.1 >nul' -WindowStyle Hidden
```

Expected result:

- While active, the card shows `Running`, elapsed time, the configured timeout,
  and `Stop cancels`.
- It returns promptly instead of waiting for the 30-second child.
- The terminal card contains `background stopped` and explains that the child
  was terminated.

### 2. Supported foreground child

```powershell
Start-Process -FilePath "$env:ComSpec" -ArgumentList '/c','ping -n 3 127.0.0.1 >nul' -Wait -WindowStyle Hidden
```

Expected result: it runs for roughly two seconds and completes normally without
the `background stopped` chip.

### 3. Cancellation

```powershell
Start-Sleep -Seconds 30
```

Press **Stop** after several seconds. Expected result: elapsed time updates
while running, then the terminal card reports `cancelled` and the UI returns to
idle.

## Automated-test note

The current `LlamaBossTests` JSON runner has no process-executor action and does
not link `CmdExecutor`, so its existing JSON cases cannot observe Windows Job
Object or inherited-pipe behavior. This patch therefore includes a focused,
deterministic manual regression procedure instead of a misleading JSON test
that only inspects source text. Existing native tests should still be run after
the project compiles.
