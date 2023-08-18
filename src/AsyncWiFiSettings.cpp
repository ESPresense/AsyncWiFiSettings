#include "AsyncWiFiSettings.h"

#define ESPFS SPIFFS
#define ESPMAC (Sprintf("%06" PRIx32, ((uint32_t)(ESP.getEfuseMac() >> 24))))

#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#include <DNSServer.h>
#include <limits.h>
#include <vector>
#include "AsyncWiFiSettings_strings.h"

AsyncWiFiSettingsLanguage::Texts _WSL_T;

#define Sprintf(f, ...) ({ char* s; asprintf(&s, f, __VA_ARGS__); String r = s; free(s); r; })

namespace {  // Helpers
    String slurp(const String &fn) {
        File f = ESPFS.open(fn, "r");
        String r = f.readString();
        f.close();
        return r;
    }

    bool spurt(const String &fn, const String &content) {
        if (content.isEmpty())
            return ESPFS.exists(fn) ? ESPFS.remove(fn) : true;
        File f = ESPFS.open(fn, "w");
        if (!f) return false;
        auto w = f.print(content);
        f.close();
        return w == content.length();
    }

    String pwgen() {
        const char *passchars = "ABCEFGHJKLMNPRSTUXYZabcdefhkmnorstvxz23456789-#@?!";
        String password = "";
        for (int i = 0; i < 16; i++) {
            // Note: no seed needed for ESP8266 and ESP32 hardware RNG
            password.concat(passchars[random(strlen(passchars))]);
        }
        return password;
    }

    String secure(const String &raw) {
        String r;
        for (unsigned int i = 0; i < raw.length(); i++) {
            r += ' ';
        }
        return r;
    }

    String html_entities(const String &raw) {
        String r;
        for (unsigned int i = 0; i < raw.length(); i++) {
            char c = raw.charAt(i);
            if (c < '!' || c == '"' || c == '&' || c == '\'' || c == '<' || c == '>' || c == 0x7f) {
                // ascii control characters, html syntax characters, and space
                r += Sprintf("&#%d;", c);
            } else {
                r += c;
            }
        }
        return r;
    }

    struct AsyncWiFiSettingsParameter {
        String name;
        String label;
        String value;
        String init;
        long min = LONG_MIN;
        long max = LONG_MAX;

        String filename() {
            String fn = "/";
            fn += name;
            return fn;
        }

        bool store() { return (name && name.length()) ? spurt(filename(), value) : true; }

        void fill() { if (name && name.length()) value = slurp(filename()); }

        virtual void set(const String &) = 0;

        virtual String html() = 0;
    };

    struct AsyncWiFiSettingsDropdown : AsyncWiFiSettingsParameter {
        virtual void set(const String &v) { value = v; }

        std::vector <String> options;

        String html() {
            String h = F("<p><label>{label}:<br><select name='{name}' value='{value}'>");
            h.replace("{name}", html_entities(name));
            h.replace("{value}", html_entities(value));
            h.replace("{label}", html_entities(label));

            int i = 0;
            if (value == "") {
                for (auto &o: options) {
                    String s = String(i);
                    if (s == init)
                    {
                        String opt = F("<option value='' disabled selected hidden>{name}</option>");
                        opt.replace("{name}", o);
                        h += opt;
                    }
                    i++;
                }
            }

            i = 0;
            for (auto &o : options)
            {
                String s = String(i);
                String opt = F("<option value='{code}'{sel}>{name}</option>");
                opt.replace("{code}", String(i));
                opt.replace("{name}", o);
                opt.replace("{sel}", s == value ? " selected" : "");
                h += opt;
                i++;
            }
            h += (F("</select></label>"));
            return h;
        }
    };

    struct AsyncWiFiSettingsString : AsyncWiFiSettingsParameter {
        virtual void set(const String &v) { value = v; }

        String html() {
            String h = F("<p><label>{label}:<br><input name='{name}' value='{value}' placeholder='{init}'></label>");
            h.replace("{name}", html_entities(name));
            h.replace("{value}", html_entities(value));
            h.replace("{init}", html_entities(init));
            h.replace("{label}", html_entities(label));
            return h;
        }
    };

    struct AsyncWiFiSettingsPassword : AsyncWiFiSettingsParameter {
        virtual void set(const String &v) {
            String trimmed = v;
            trimmed.trim();
            if (trimmed.length()) value = trimmed;
        }

        String html() {
            String h = F("<p><label>{label}:<br><input type='password' name='{name}' value='{value}' placeholder='{init}'></label>");
            h.replace("{name}", html_entities(name));
            h.replace("{value}", html_entities(secure(value)));
            h.replace("{init}", html_entities(init));
            h.replace("{label}", html_entities(label));
            return h;
        }
    };

    struct AsyncWiFiSettingsInt : AsyncWiFiSettingsParameter {
        virtual void set(const String &v) { value = v; }

        String html() {
            String h = F(
                    "<p><label>{label}:<br><input type=number step=1 min={min} max={max} name='{name}' value='{value}' placeholder='{init}'></label>");
            h.replace("{name}", html_entities(name));
            h.replace("{value}", html_entities(value));
            h.replace("{init}", html_entities(init));
            h.replace("{label}", html_entities(label));
            h.replace("{min}", String(min));
            h.replace("{max}", String(max));
            return h;
        }
    };

    struct AsyncWiFiSettingsFloat : AsyncWiFiSettingsParameter {
        virtual void set(const String &v) { value = v; }

        String html() {
            String h = F(
                    "<p><label>{label}:<br><input type=number step=0.01 min={min} max={max} name='{name}' value='{value}' placeholder='{init}'></label>");
            h.replace("{name}", html_entities(name));
            h.replace("{value}", html_entities(value));
            h.replace("{init}", html_entities(init));
            h.replace("{label}", html_entities(label));
            h.replace("{min}", String(min));
            h.replace("{max}", String(max));
            return h;
        }
    };

    struct AsyncWiFiSettingsBool : AsyncWiFiSettingsParameter {
        virtual void set(const String &v) { value = v.length() ? "1" : "0"; }

        String html() {
            String h = F(
                    "<p><label class=c><input type=checkbox name='{name}' value=1{checked}> {label} ({default}: {init})</label>");
            h.replace("{name}", html_entities(name));
            h.replace("{default}", _WSL_T.init);
            h.replace("{checked}", value.toInt() ? " checked" : "");
            h.replace("{init}", init.toInt() ? "&#x2611;" : "&#x2610;");
            h.replace("{label}", html_entities(label));
            return h;
        }
    };

    struct AsyncWiFiSettingsHTML : AsyncWiFiSettingsParameter {
        // Raw HTML, not an actual parameter. The reason for the "if (name)"
        // in store and fill. Abuses several member variables for completely
        // different functionality.

        virtual void set(const String &v) { (void) v; }

        String html() {
            int space = value.indexOf(" ");

            String h =
                    (value ? "<" + value + ">" : "") +
                    (min ? html_entities(label) : label) +
                    (value ? "</" + (space >= 0 ? value.substring(0, space) : value) + ">" : "");
            return h;
        }
    };

    struct std::vector<AsyncWiFiSettingsParameter *> params;
}

String AsyncWiFiSettingsClass::pstring(const String &name, const String &init, const String &label) {
    begin();
    struct AsyncWiFiSettingsPassword *x = new AsyncWiFiSettingsPassword();
    x->name = name;
    x->label = label.length() ? label : name;
    x->init = init;
    x->fill();

    params.push_back(x);
    return x->value.length() ? x->value : x->init;
}

String AsyncWiFiSettingsClass::string(const String &name, const String &init, const String &label) {
    begin();
    struct AsyncWiFiSettingsString *x = new AsyncWiFiSettingsString();
    x->name = name;
    x->label = label.length() ? label : name;
    x->init = init;
    x->fill();

    params.push_back(x);
    return x->value.length() ? x->value : x->init;
}

String AsyncWiFiSettingsClass::string(const String &name, unsigned int max_length, const String &init, const String &label) {
    String rv = string(name, init, label);
    params.back()->max = max_length;
    return rv;
}

String
AsyncWiFiSettingsClass::string(const String &name, unsigned int min_length, unsigned int max_length, const String &init,
                          const String &label) {
    String rv = string(name, init, label);
    params.back()->min = min_length;
    params.back()->max = max_length;
    return rv;
}

long AsyncWiFiSettingsClass::dropdown(const String &name, std::vector <String> options, long init, const String &label) {
    begin();
    struct AsyncWiFiSettingsDropdown *x = new AsyncWiFiSettingsDropdown();
    x->name = name;
    x->label = label.length() ? label : name;
    x->init = init;
    x->options = options;
    x->fill();

    params.push_back(x);
    return (x->value.length() ? x->value : x->init).toInt();
}

long AsyncWiFiSettingsClass::integer(const String &name, long init, const String &label) {
    begin();
    struct AsyncWiFiSettingsInt *x = new AsyncWiFiSettingsInt();
    x->name = name;
    x->label = label.length() ? label : name;
    x->init = init;
    x->fill();

    params.push_back(x);
    return (x->value.length() ? x->value : x->init).toInt();
}

long AsyncWiFiSettingsClass::integer(const String &name, long min, long max, long init, const String &label) {
    long rv = integer(name, init, label);
    params.back()->min = min;
    params.back()->max = max;
    return rv;
}

float AsyncWiFiSettingsClass::floating(const String &name, float init, const String &label) {
    begin();
    struct AsyncWiFiSettingsFloat *x = new AsyncWiFiSettingsFloat();
    x->name = name;
    x->label = label.length() ? label : name;
    x->init = init;
    x->fill();

    params.push_back(x);
    return (x->value.length() ? x->value : x->init).toFloat();
}

float AsyncWiFiSettingsClass::floating(const String &name, long min, long max, float init, const String &label) {
    float rv = floating(name, init, label);
    params.back()->min = min;
    params.back()->max = max;
    return rv;
}

bool AsyncWiFiSettingsClass::checkbox(const String &name, bool init, const String &label) {
    begin();
    struct AsyncWiFiSettingsBool *x = new AsyncWiFiSettingsBool();
    x->name = name;
    x->label = label.length() ? label : name;
    x->init = String((int) init);
    x->fill();

    // Apply default immediately because a checkbox has no placeholder to
    // show the default, and other UI elements aren't sufficiently pretty.
    if (!x->value.length()) x->value = x->init;

    params.push_back(x);
    return x->value.toInt();
}

void AsyncWiFiSettingsClass::html(const String &tag, const String &contents, bool escape) {
    begin();
    struct AsyncWiFiSettingsHTML *x = new AsyncWiFiSettingsHTML();
    x->value = tag;
    x->label = contents;
    x->min = escape;

    params.push_back(x);
}

void AsyncWiFiSettingsClass::info(const String &contents, bool escape) {
    html(F("p class=i"), contents, escape);
}

void AsyncWiFiSettingsClass::warning(const String &contents, bool escape) {
    html(F("p class=w"), contents, escape);
}

void AsyncWiFiSettingsClass::heading(const String &contents, bool escape) {
    html("h2", contents, escape);
}

void AsyncWiFiSettingsClass::httpSetup(bool wifi) {
    begin();

    static int num_networks = -1;
    static String ip = WiFi.softAPIP().toString();
    static bool configureWifi = wifi;

    if (onHttpSetup) onHttpSetup(&http);

    auto redirect = [](AsyncWebServerRequest *request) {
        if (!configureWifi) return false;
        // iPhone doesn't deal well with redirects to http://hostname/ and
        // will wait 40 to 60 seconds before succesful retry. Works flawlessly
        // with http://ip/ though.
        if (request->host() == ip) return false;

        request->redirect("http://" + ip + "/");
        return true;
    };

    http.on("/", HTTP_GET, [this,redirect](AsyncWebServerRequest *request) {
        if (redirect(request)) return;

        AsyncResponseStream *response = request->beginResponseStream("text/html");

        bool interactive = false;
        if (request->hasHeader("User-Agent")) {
            AsyncWebHeader *h = request->getHeader("User-Agent");
            String ua = h->value();
            if (onUserAgent) onUserAgent(ua);
            interactive = !ua.startsWith(F("CaptiveNetworkSupport"));
        }

        if (interactive && onPortalView) onPortalView();

        response->print(F("<!DOCTYPE html>\n<title>"));
        response->print(html_entities(hostname));
        response->print(F("</title><meta name=viewport content='width=device-width,initial-scale=1'>"
                          "<style>"
                          "*{box-sizing:border-box} "
                          "html{background:#444;font:10pt sans-serif;width:100vw;max-width:100%} "
                          "body{background:#ccc;color:black;max-width:30em;padding:1em;margin:1em auto}"
                          "a:link{color:#000;text-decoration: none} "
                          "label{clear:both}"
                          "select,input:not([type^=c]){display:block;width:100%;border:1px solid #444;padding:.3ex}"
                          "input[type^=s]{display:inline;width:auto;background:#de1;padding:1ex;border:1px solid #000;border-radius:1ex}"
                          "[type^=c]{float:left;margin-left:-1.5em}"
                          ":not([type^=s]):focus{outline:2px solid #d1ed1e}"
                          ".w::before{content:'\\26a0\\fe0f'}"
                          "p::before{margin-left:-2em;float:left;padding-top:1ex}"
                          ".i::before{content:'\\2139\\fe0f'}"
                          ".c{display:block;padding-left:2em}"
                          ".w,.i{display:block;padding:.5ex .5ex .5ex 3em}"
                          ".w,.i{background:#aaa;min-height:3em}"
                          "</style>"
                          "<form action=/restart method=post>"));
        response->print(F("<input type=submit value=\""));
        response->print(_WSL_T.button_restart);
        response->print(F("\"></form><hr><h1>"));
        response->print(_WSL_T.title);
        response->print(F("</h1><form method=post><label>"));

        // Don't waste time scanning in captive portal detection (Apple)
        if (configureWifi && interactive) {
            response->print(_WSL_T.ssid);
            response->print(F(":<br><b class=s>"));
            response->print(_WSL_T.scanning_long);
            response->print("</b>");
            if (num_networks < 0) num_networks = WiFi.scanNetworks();
            Serial.print(num_networks, DEC);
            Serial.println(F(" WiFi networks found."));

            response->print(F(
                    "<style>.s{display:none}</style>" // hide "scanning"
                    "<select name=ssid onchange=\"document.getElementsByName('password')[0].value=''\">"));

            String current = slurp("/wifi-ssid");
            bool found = false;
            for (int i = 0; i < num_networks; i++) {
                String opt = F("<option value='{ssid}'{sel}>{ssid} {lock} {1x}</option>");
                String ssid = WiFi.SSID(i);
                wifi_auth_mode_t mode = WiFi.encryptionType(i);

                opt.replace("{sel}", ssid == current && !found ? " selected" : "");
                opt.replace("{ssid}", html_entities(ssid));
                opt.replace("{lock}", mode != WIFI_AUTH_OPEN ? "&#x1f512;" : "");
                opt.replace("{1x}", mode == WIFI_AUTH_WPA2_ENTERPRISE ? _WSL_T.dot1x : F(""));
                response->print(opt);

                if (ssid == current) found = true;
            }
            if (!found && current.length()) {
                String opt = F("<option value='{ssid}' selected>{ssid} (&#x26a0; not in range)</option>");
                opt.replace("{ssid}", html_entities(current));
                response->print(opt);
            }

            response->print(F("</select></label> <a href=/rescan onclick=\"this.innerHTML='"));
            response->print(_WSL_T.scanning_short);
            response->print("';\">");
            response->print(_WSL_T.rescan);
            response->print(F("</a><p><label>"));

            response->print(_WSL_T.wifi_password);
            response->print(F(":<br><input name=password value='"));
            if (slurp("/wifi-password").length()) response->print("##**##**##**");
            response->print(F("'></label><hr>"));
        }

        if (AsyncWiFiSettingsLanguage::multiple()) {
            response->print(F("<label>"));
            response->print(_WSL_T.language);
            response->print(F(":<br><select name=language>"));

            for (auto &lang: AsyncWiFiSettingsLanguage::languages) {
                String opt = F("<option value='{code}'{sel}>{name}</option>");
                opt.replace("{code}", lang.first);
                opt.replace("{name}", lang.second);
                opt.replace("{sel}", language == lang.first ? " selected" : "");
                response->print(opt);
            }
            response->print(F("</select></label>"));
        }

        for (auto &p: params) {
            response->print(p->html());
        }

        response->print(F(
                "<p style='position:sticky;bottom:0;text-align:right'>"
                "<input type=submit value=\""));
        response->print(_WSL_T.button_save);
        response->print(F("\"style='font-size:150%'></form>"));
        request->send(response);
    });

    http.on("/", HTTP_POST, [this](AsyncWebServerRequest *request) {
        bool ok = true;

        if (configureWifi) {
            auto ssid = request->arg("ssid");
            if (!ssid.isEmpty()) {
                if (!spurt("/wifi-ssid", ssid)) ok = false;
            }

            String pw = request->arg("password");
            if (!pw.isEmpty() && pw != "##**##**##**") {
                if (!spurt("/wifi-password", pw)) ok = false;
            }
        }

        if (AsyncWiFiSettingsLanguage::multiple()) {
            if (!spurt("/AsyncWiFiSettings-language", request->arg("language"))) ok = false;
            // Don't update immediately, because there is currently
            // no mechanism for reloading param strings.
            //language = request->arg("language");
            //AsyncWiFiSettingsLanguage::select(T, language);
        }


        for (auto &p: params) {
            p->set(request->arg(p->name));
            if (!p->store()) ok = false;
        }

        if (ok) {
            request->redirect("/");
            if (onConfigSaved) onConfigSaved();
        } else {
            // Could be missing SPIFFS.begin(), unformatted filesystem, or broken flash.
            request->send(500, "text/plain", _WSL_T.error_fs);
        }
    });

    http.on("/restart", HTTP_POST, [this](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", _WSL_T.bye);
        if (onRestart) onRestart();
        ESP.restart();
    });

    http.on("/rescan", HTTP_GET, [this](AsyncWebServerRequest *request) {
        request->redirect("/");
        num_networks = WiFi.scanNetworks();
    });

    http.onNotFound([this, &redirect](AsyncWebServerRequest *request) {
        if (redirect(request)) return;
        request->send(404, "text/plain", "404");
    });
    http.begin();
}

void AsyncWiFiSettingsClass::portal() {
    begin();

#ifdef ESP32
    WiFi.disconnect(true, true);    // reset state so .scanNetworks() works
#else
    WiFi.disconnect(true);
#endif
    WiFi.mode(WIFI_AP);

    Serial.println(F("Starting access point for configuration portal."));
    if (secure && password.length()) {
        Serial.printf("SSID: '%s', Password: '%s'\n", hostname.c_str(), password.c_str());
        if (!WiFi.softAP(hostname.c_str(), password.c_str()))
            Serial.println("Failed to start access point!");
    } else {
        Serial.printf("SSID: '%s'\n", hostname.c_str());
        if (!WiFi.softAP(hostname.c_str()))
            Serial.println("Failed to start access point!");
    }
    delay(500);
    DNSServer dns;
    dns.setTTL(0);
    dns.start(53, "*", WiFi.softAPIP());

    if (onPortal) onPortal();
    String ip = WiFi.softAPIP().toString();
    Serial.printf("IP: %s\n", ip.c_str());

    httpSetup(true);

    unsigned long starttime = millis();
    int desired = 0;
    for (;;)
    {
        dns.processNextRequest();
        if (onPortalWaitLoop && (millis() - starttime) > desired) {
            desired = onPortalWaitLoop();
            starttime = millis();
        }
        esp_task_wdt_reset();
        delay(1);
    }
}

bool AsyncWiFiSettingsClass::connect(bool portal, int wait_seconds) {
    begin();

    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.mode(WIFI_OFF);
    }

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);

    String ssid = slurp("/wifi-ssid");
    String pw = slurp("/wifi-password");
    if (ssid.length() == 0) {
        Serial.println(F("First contact!\n"));
        this->portal();
    }

    Serial.print(F("Connecting to WiFi SSID '"));
    Serial.print(ssid);
    Serial.print(F("'"));
    if (onConnect) onConnect();

    WiFi.setHostname(hostname.c_str());
    auto status = WiFi.begin(ssid.c_str(), pw.c_str());

    unsigned long wait_ms = wait_seconds * 1000UL;
    unsigned long starttime = millis();
    unsigned long lastbegin = starttime;
    while (status != WL_CONNECTED) {
        // Reconnect every 60 seconds
        if (millis() - lastbegin > 60000) {
            lastbegin = millis();
            Serial.print("*");
            WiFi.disconnect(true, true);
            status = WiFi.begin(ssid.c_str(), pw.c_str());
        } else {
            Serial.print(".");
            status = WiFi.status();
        }
        delay(onWaitLoop ? onWaitLoop() : 100);
        if (wait_seconds >= 0 && millis() - starttime > wait_ms)
            break;
    }

    if (status != WL_CONNECTED) {
        Serial.printf(" failed (status=%d).\n", status);
        if (onFailure) onFailure();
        if (portal) this->portal();
        return false;
    }

    Serial.println(WiFi.localIP().toString());
    if (onSuccess) onSuccess();
    return true;
}

void AsyncWiFiSettingsClass::begin() {
    if (begun) return;
    begun = true;

    // These things can't go in the constructor because the constructor runs
    // before ESPFS.begin()

    String user_language = slurp("/AsyncWiFiSettings-language");
    user_language.trim();
    if (user_language.length() && AsyncWiFiSettingsLanguage::available(user_language)) {
        language = user_language;
    }
    AsyncWiFiSettingsLanguage::select(_WSL_T, language);  // can update language

#ifdef PORTAL_PASSWORD

    if (!secure) {
        secure = checkbox(
            F("AsyncWiFiSettings-secure"),
            false,
            _WSL_T.portal_wpa
        );
    }

    if (!password.length()) {
        password = string(
            F("AsyncWiFiSettings-password"),
            8, 63,
            "",
            _WSL_T.portal_password
        );
        if (password == "") {
            // With regular 'init' semantics, the password would be changed
            // all the time.
            password = pwgen();
            params.back()->set(password);
            params.back()->store();
        }
    }

#endif

    if (hostname.endsWith("-")) hostname += ESPMAC;
}

AsyncWiFiSettingsClass::AsyncWiFiSettingsClass() : http(80) {
#ifdef ESP32
    hostname = F("esp32-");
#else
    hostname = F("esp8266-");
#endif

    language = "en";
}

AsyncWiFiSettingsClass AsyncWiFiSettings;
