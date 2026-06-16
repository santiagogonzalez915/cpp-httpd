#!/bin/sh
LOG=/tmp/cpp-httpd-demo-access.log
NOW=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
REQS=$(wc -l < "$LOG" 2>/dev/null | tr -d ' \t')
[ -z "$REQS" ] && REQS=0

printf "Content-Type: text/html; charset=utf-8\r\n"
printf "\r\n"

cat <<HTML
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Server status — cpp-httpd</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
<nav>
  <a class="logo" href="/">cpp-httpd</a>
  <a href="/static/">static</a>
  <a href="/cgi-bin/status.sh">status</a>
  <a href="/cgi-bin/inspect.sh">inspector</a>
  <a href="/admin/">admin</a>
  <a href="/files/">files</a>
</nav>

<main>
  <h1>Server status</h1>
  <p>
    This page is generated dynamically by <code>status.sh</code> via
    <code>CgiHandler</code> — a <code>fork</code>/<code>exec</code> call
    spawns this shell script. Every reload hits the server and runs the script.
  </p>

  <h2>Runtime</h2>
  <div class="stat-grid">
    <div class="stat-card">
      <div class="stat-label">time (UTC)</div>
      <div class="stat-value" style="font-size:15px">$NOW</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">requests served</div>
      <div class="stat-value">$REQS</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">cgi pid</div>
      <div class="stat-value">$$</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">server port</div>
      <div class="stat-value">${SERVER_PORT:-6789}</div>
    </div>
  </div>

  <h2>CGI environment</h2>
  <table>
    <thead><tr><th>Variable</th><th>Value</th></tr></thead>
    <tbody>
      <tr><td>REQUEST_METHOD</td><td><code>${REQUEST_METHOD}</code></td></tr>
      <tr><td>SERVER_NAME</td><td><code>${SERVER_NAME}</code></td></tr>
      <tr><td>SERVER_PORT</td><td><code>${SERVER_PORT}</code></td></tr>
      <tr><td>SERVER_PROTOCOL</td><td><code>${SERVER_PROTOCOL}</code></td></tr>
      <tr><td>REMOTE_ADDR</td><td><code>${REMOTE_ADDR}</code></td></tr>
      <tr><td>QUERY_STRING</td><td><code>${QUERY_STRING}</code></td></tr>
    </tbody>
  </table>

  <h2>How it works</h2>
  <p>
    <code>CgiHandler</code> forks a child process, pipes the request body to
    its stdin, reads stdout (with a configurable timeout — returns 504 on hang),
    and forwards the output as the HTTP response body. The server enforces a
    subprocess timeout using <code>select()</code> on the child's stdout fd.
  </p>
  <pre>fork() → execve(status.sh) → read stdout → HTTP response</pre>
</main>
</body>
</html>
HTML
