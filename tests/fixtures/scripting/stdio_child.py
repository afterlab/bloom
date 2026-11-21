"""The stdio servers a fixture starts (bloom-mcp), started and ended one way.

A server's standard error goes to a file, never to a pipe nobody reads: a server that writes a lot
can't block on a full pipe, and a failure can show what it wrote. A server is always ended. One
that doesn't exit after its input closes is killed, because it is this fixture's own child and must
not outlive it, and the failure names it and shows its standard error.
"""
import subprocess
import tempfile


def start(arguments, **options):
    """Starts a server with piped input and output and its standard error in a file."""
    errors = tempfile.TemporaryFile()
    try:
        process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors, **options)
    except BaseException:
        errors.close()
        raise
    process.error_file = errors
    return process


def errors(process):
    """What the server has written to its standard error so far (empty once it was ended)."""
    if process.error_file.closed:
        return ''
    process.error_file.flush()
    process.error_file.seek(0)
    return process.error_file.read().decode(errors='replace')


def end(process, timeout, expect_status=None):
    """Closes the server's input and waits for it to exit; returns its exit status.

    A server still running `timeout` seconds after its input closed is killed, and the
    AssertionError names it with its standard error. With `expect_status`, another exit status is
    an AssertionError with the standard error too. A server already ended returns its status."""
    if process.error_file.closed:
        return process.wait()
    try:
        process.stdin.close()
    except OSError:
        pass  # The server is already gone; wait() says how it ended.
    try:
        status = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        written = errors(process)
        _release(process)
        raise AssertionError(
            f'{process.args[0]} did not exit within {timeout} s after its input closed, so the '
            f'test killed it. Its standard error:\n{written}') from None
    written = errors(process)
    _release(process)
    if expect_status is not None and status != expect_status:
        raise AssertionError(f'{process.args[0]} exited with status {status}, not '
                             f'{expect_status}. Its standard error:\n{written}')
    return status


def kill(process):
    """Ends a server that a failed case left unusable."""
    if process.error_file.closed:
        return
    process.kill()
    process.wait()
    _release(process)


def _release(process):
    for stream in (process.stdin, process.stdout):
        try:
            stream.close()
        except OSError:
            pass
    process.error_file.close()
