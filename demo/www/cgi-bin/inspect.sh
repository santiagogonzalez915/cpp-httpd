#!/bin/sh
printf "Content-Type: text/html; charset=utf-8\r\n"
printf "\r\n"

cat <<HTML
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Request inspector — cpp-httpd</title>
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
  <h1>Request inspector</h1>
  <p>
    This page echoes back what the server parsed from your HTTP request.
    The values below come from the CGI environment that
    <code>CgiHandler::build_cgi_environment()</code> populated — proving
    the parser extracted them correctly.
  </p>

  <h2>Parsed request</h2>
  <table>
    <thead><tr><th>Field</th><th>Value</th></tr></thead>
    <tbody>
      <tr><td>Method</td><td><code>${REQUEST_METHOD}</code></td></tr>
      <tr><td>Protocol</td><td><code>${SERVER_PROTOCOL}</code></td></tr>
      <tr><td>Query string</td><td><code>${QUERY_STRING:-&lt;empty&gt;}</code></td></tr>
      <tr><td>Content-Length</td><td><code>${CONTENT_LENGTH:-0}</code></td></tr>
      <tr><td>Content-Type</td><td><code>${CONTENT_TYPE:-&lt;none&gt;}</code></td></tr>
    </tbody>
  </table>

  <h2>Connection</h2>
  <table>
    <thead><tr><th>Field</th><th>Value</th></tr></thead>
    <tbody>
      <tr><td>Remote address</td><td><code>${REMOTE_ADDR}</code></td></tr>
      <tr><td>Server name</td><td><code>${SERVER_NAME}</code></td></tr>
      <tr><td>Server port</td><td><code>${SERVER_PORT}</code></td></tr>
    </tbody>
  </table>

  <h2>Try it with a query string</h2>
  <p>Append a query string to this URL and reload:</p>
  <pre>http://localhost:6789/cgi-bin/inspect.sh?foo=bar&amp;baz=42</pre>
  <p>QUERY_STRING above will update to show the parsed value.</p>

  <div class="callout">
    <strong>How it works:</strong> <code>RequestParser</code> splits the URI on
    <code>?</code> to extract the query string before routing. The path is
    resolved against the DocumentRoot; the query string is passed verbatim
    via the <code>QUERY_STRING</code> CGI variable.
  </div>
</main>
</body>
</html>
HTML
