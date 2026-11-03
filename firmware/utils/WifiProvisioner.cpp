/*
Purpose: WifiProvisioner — captive-portal AP for first-boot WiFi setup.
See WifiProvisioner.h for full usage notes.
*/
#include "utils/WifiProvisioner.h"
#include "config/RuntimeConfig.h"
#include "utils/TelnetLogger.h"
#include <Preferences.h>
#include <vector>
#include <math.h>

static constexpr const char *kNS = "fw_wifi";

// ────────────────────────────────────────────────────────────────────────────
// Credential helpers
// ────────────────────────────────────────────────────────────────────────────

bool WifiProvisioner::hasCredentials()
{
    String s, p;
    return loadCredentials(s, p);
}

bool WifiProvisioner::loadCredentials(String &ssid, String &password)
{
    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/true)) return false;
    if (!p.isKey("ssid"))                { p.end(); return false; }
    ssid     = p.getString("ssid", "");
    password = p.getString("pass", "");
    p.end();
    return ssid.length() > 0;
}

void WifiProvisioner::saveCredentials(const String &ssid, const String &password)
{
    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/false)) return;
    p.putString("ssid", ssid);
    p.putString("pass", password);
    p.end();
}

void WifiProvisioner::clearCredentials()
{
    Preferences p;
    if (!p.begin(kNS, /*readOnly=*/false)) return;
    p.clear();
    p.end();
}

// ────────────────────────────────────────────────────────────────────────────
// HTTP helpers
// ────────────────────────────────────────────────────────────────────────────

void WifiProvisioner::sendHtml(WiFiClient &c, int code, const String &html)
{
    const char *reason = (code == 200) ? "OK" : "Bad Request";
    c.printf("HTTP/1.1 %d %s\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %u\r\n"
             "Cache-Control: no-cache\r\n"
             "Connection: close\r\n\r\n",
             code, reason, (unsigned)html.length());
    // Send in 512-byte chunks
    const char *p = html.c_str();
    size_t      rem = html.length();
    while (rem > 0)
    {
        size_t chunk = rem < 512 ? rem : 512;
        c.write((const uint8_t *)p, chunk);
        p += chunk;
        rem -= chunk;
    }
}

void WifiProvisioner::sendRedirect(WiFiClient &c, const String &location)
{
    c.printf("HTTP/1.1 302 Found\r\nLocation: %s\r\n"
             "Content-Length: 0\r\nConnection: close\r\n\r\n",
             location.c_str());
}

// ────────────────────────────────────────────────────────────────────────────
// Form parsing
// ────────────────────────────────────────────────────────────────────────────

String WifiProvisioner::urlDecode(const String &s)
{
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i)
    {
        char c = s[i];
        if (c == '+')
        {
            out += ' ';
        }
        else if (c == '%' && i + 2 < s.length())
        {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        }
        else
        {
            out += c;
        }
    }
    return out;
}

String WifiProvisioner::formField(const String &body, const String &key)
{
    String k   = key + "=";
    int    pos = body.indexOf(k);
    if (pos < 0) return "";
    int start = pos + k.length();
    int end   = body.indexOf('&', start);
    return urlDecode(end < 0 ? body.substring(start)
                             : body.substring(start, end));
}

// ────────────────────────────────────────────────────────────────────────────
// HTML page builders
// ────────────────────────────────────────────────────────────────────────────

String WifiProvisioner::buildPage(const String &apName,
                                  const std::vector<String> &nets)
{
    String opts;
    for (const auto &n : nets)
        opts += "<option value=\"" + n + "\">";

    return String(
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Glideslope Setup</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#090c11;color:#e4e9f0;font:14px/1.5 system-ui,sans-serif;"
             "display:flex;justify-content:center;padding:28px 16px}"
        ".card{background:#10151d;border:1px solid #1d2531;border-radius:14px;"
              "padding:24px;width:100%;max-width:420px}"
        ".w{font-weight:800;letter-spacing:.16em;font-size:14px;margin-bottom:18px}.w i{font-style:normal;color:#ffb93c}"
        "h1{font-size:20px;font-weight:650;margin-bottom:4px}"
        "p.sub{color:#8d99aa;font-size:13px;margin-bottom:8px}"
        "h2{font-size:12px;color:#8d99aa;text-transform:uppercase;font-weight:650;"
            "letter-spacing:.08em;margin:22px 0 10px;display:flex;align-items:center;gap:8px}"
        "h2::before{content:'';width:3px;height:12px;border-radius:1px;background:#ffb93c}"
        ".f{margin-bottom:12px}"
        "label{display:block;font-size:13px;font-weight:550;margin-bottom:6px}"
        "input{width:100%;background:#0a0e14;border:1px solid #2a3443;"
              "color:#e4e9f0;padding:10px 12px;border-radius:10px;font:inherit}"
        "input:focus{outline:none;border-color:#ffb93c;box-shadow:0 0 0 3px rgba(255,185,60,.16)}"
        ".r2{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
        "button{width:100%;background:#ffb93c;color:#1a1205;border:none;"
               "padding:12px;border-radius:10px;font:inherit;font-weight:700;font-size:15px;"
               "cursor:pointer;margin-top:20px}"
        "button:hover{background:#ffcd6b}"
        ".hint{font-size:12px;color:#5c6878;text-align:center;margin-top:14px}"
        "</style></head>"
        "<body><div class=\"card\">"
        "<div class=\"w\">GLIDE<i>SLOPE</i></div>"
        "<h1>Set up your board</h1>"
        "<p class=\"sub\">Your Wi-Fi and where you live, and it will start watching the sky.</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<h2>Wi-Fi</h2>"
        "<div class=\"f\"><label>Network (SSID)</label>"
        "<input name=\"ssid\" list=\"nl\" required autocomplete=\"off\""
               " placeholder=\"Select or type your network name\">"
        "<datalist id=\"nl\">" + opts + "</datalist></div>"
        "<div class=\"f\"><label>Password</label>"
        "<input name=\"pass\" type=\"password\" autocomplete=\"new-password\""
               " placeholder=\"Leave blank for open networks\"></div>"
        "<h2>Location</h2>"
        "<div class=\"r2\">"
        "<div class=\"f\"><label>Latitude</label>"
        "<input name=\"lat\" type=\"number\" step=\"any\" required placeholder=\"e.g. 51.5\"></div>"
        "<div class=\"f\"><label>Longitude</label>"
        "<input name=\"lon\" type=\"number\" step=\"any\" required placeholder=\"e.g. -0.12\"></div>"
        "</div>"
        "<div class=\"f\"><label>Search radius (km)</label>"
        "<input name=\"radius\" type=\"number\" step=\"1\" min=\"1\" value=\"20\"></div>"
        "<div class=\"f\"><label>UTC offset (hours, e.g. -5 or 5.5)</label>"
        "<input name=\"utc\" type=\"number\" step=\"0.25\" min=\"-12\" max=\"14\" value=\"0\"></div>"
        "<button type=\"submit\">Save and connect</button>"
        "</form>"
        "<p class=\"hint\">AP: " + apName + " &bull; 192.168.4.1</p>"
        "</div></body></html>"
    );
}

String WifiProvisioner::buildSavedPage(const String &ssid)
{
    return String(
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Glideslope &mdash; Saved</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#090c11;color:#e4e9f0;font:14px/1.5 system-ui,sans-serif;"
             "display:flex;justify-content:center;padding:40px 16px}"
        ".card{background:#10151d;border:1px solid #1d2531;border-radius:14px;"
              "padding:32px 24px;width:100%;max-width:380px;text-align:center}"
        ".ok{width:48px;height:48px;line-height:48px;margin:0 auto 14px;border-radius:50%;background:rgba(255,185,60,.14);color:#ffb93c;font-size:24px}"
        "h1{font-size:18px;font-weight:650;margin-bottom:10px}"
        "p{color:#8d99aa;font-size:13px}"
        "</style></head>"
        "<body><div class=\"card\">"
        "<div class=\"ok\">&#10003;</div>"
        "<h1>Connecting to " + ssid + "</h1>"
        "<p>Glideslope is restarting and will join your network.<br><br>"
        "Reconnect your device to <strong>" + ssid + "</strong> "
        "then browse to the Glideslope IP for the live config panel.</p>"
        "</div></body></html>"
    );
}

// ────────────────────────────────────────────────────────────────────────────
// Main provisioning loop
// ────────────────────────────────────────────────────────────────────────────

void WifiProvisioner::run(const String &apName)
{
    status("Scanning WiFi...");
    Log.println("WifiProvisioner: scanning for networks...");

    // Scan in STA mode before switching to AP (avoids mode-switch interference)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
    std::vector<String> nets;
    for (int i = 0; i < n && i < 20; ++i)
        nets.push_back(WiFi.SSID(i));
    WiFi.scanDelete();
    Log.printf("WifiProvisioner: found %d networks\n", n);

    // ── Start AP ──────────────────────────────────────────────────────────────
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str());
    delay(500);   // AP needs a moment to initialise

    IPAddress apIP = WiFi.softAPIP();
    Log.printf("WifiProvisioner: AP ready — SSID: \"%s\"  IP: %s\n",
               apName.c_str(), apIP.toString().c_str());

    // Show SSID then IP alternating on the display so the user can see both
    status(("AP: " + apName).c_str());

    // ── DNS: redirect every query to our IP (captive portal trick) ─────────────
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", apIP);

    _server.begin();

    String page = buildPage(apName, nets);

    // ── Service loop ──────────────────────────────────────────────────────────
    unsigned long lastStatusFlip = millis();
    bool          showIp         = false;

    for (;;)
    {
        _dns.processNextRequest();

        // Alternate display between SSID and IP every 3 s
        if (millis() - lastStatusFlip >= 3000UL)
        {
            lastStatusFlip = millis();
            showIp = !showIp;
            status(showIp ? "192.168.4.1" : ("AP: " + apName).c_str());
        }

        WiFiClient client = _server.accept();
        if (!client) { delay(5); continue; }

        // ── Read request headers ──────────────────────────────────────────────
        String       raw;
        bool         headersRead = false;
        unsigned long t0         = millis();
        raw.reserve(512);

        while (client.connected() && millis() - t0 < 2000 && !headersRead)
        {
            while (client.available() && !headersRead)
            {
                raw += (char)client.read();
                if (raw.endsWith("\r\n\r\n"))
                    headersRead = true;
            }
            if (!headersRead) delay(1);
        }
        if (!headersRead) { client.stop(); continue; }

        // ── Parse request line ────────────────────────────────────────────────
        int sp1 = raw.indexOf(' ');
        int sp2 = raw.indexOf(' ', sp1 + 1);
        if (sp1 < 0 || sp2 < 0) { client.stop(); continue; }

        String method   = raw.substring(0, sp1);
        String fullPath = raw.substring(sp1 + 1, sp2);
        String path     = fullPath;
        int    qp       = path.indexOf('?');
        if (qp >= 0) path = path.substring(0, qp);

        // ── Read POST body ────────────────────────────────────────────────────
        String body;
        if (method == "POST")
        {
            int clIdx = raw.indexOf("Content-Length: ");
            if (clIdx >= 0)
            {
                int clEnd   = raw.indexOf("\r\n", clIdx + 16);
                int bodyLen = raw.substring(clIdx + 16, clEnd).toInt();
                if (bodyLen > 0 && bodyLen < 4096)
                {
                    body.reserve(bodyLen + 1);
                    unsigned long bt = millis();
                    while ((int)body.length() < bodyLen && millis() - bt < 3000)
                    {
                        if (client.available()) body += (char)client.read();
                        else delay(1);
                    }
                }
            }
        }

        // ── Route ─────────────────────────────────────────────────────────────
        if (path == "/save" && method == "POST")
        {
            String ssid     = formField(body, "ssid");
            String pass     = formField(body, "pass");
            String latStr   = formField(body, "lat");
            String lonStr   = formField(body, "lon");
            String radStr   = formField(body, "radius");
            String utcStr   = formField(body, "utc");

            if (ssid.length() == 0)
            {
                sendHtml(client, 400,
                         "<h2>SSID is required. <a href=\"/\">Go back</a></h2>");
                client.flush();
                client.stop();
                continue;
            }

            // Persist WiFi credentials
            saveCredentials(ssid, pass);

            // Apply location / timezone directly into g_config and save
            if (latStr.length()) g_config.center_lat = latStr.toDouble();
            if (lonStr.length()) g_config.center_lon = lonStr.toDouble();
            if (radStr.length() && radStr.toFloat() > 0)
                g_config.radius_km = radStr.toDouble();
            if (utcStr.length())
                g_config.utc_offset_minutes =
                    (int32_t)roundf(utcStr.toFloat() * 60.0f);
            saveConfig();

            Log.printf("WifiProvisioner: credentials saved (SSID: \"%s\") — restarting\n",
                       ssid.c_str());
            status("Saved! Restarting...");

            sendHtml(client, 200, buildSavedPage(ssid));
            client.flush();
            client.stop();
            delay(500);

            _dns.stop();
            _server.stop();
            WiFi.softAPdisconnect(true);

            ESP.restart();
            // unreachable
        }
        else
        {
            // All other paths (including OS captive-portal probes) → setup page.
            // The OS sees a 200 (not the expected 204/connecttest) and pops up the
            // "Sign in to network" dialog, bringing the user straight to our form.
            sendHtml(client, 200, page);
        }

        client.flush();
        client.stop();
    }
}
