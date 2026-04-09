#pragma once
// WEB.h – WiFi-Konfigurationsseite für bTn Wecker
// Wird von runWifiConfigServer() verwendet.
// Der ESP32 startet als Access Point (SSID: WIFI_AP_SSID).
// Der Nutzer verbindet sich und öffnet 192.168.4.1 im Browser.

// ── Konfigurationsseite ──────────────────────────────────────
// Enthält clientseitige Validierung (JS) und serverseitige
// Validierung in runWifiConfigServer(). Optionaler URL-Parameter
// ?err=<Text> wird per JS als Fehlermeldung angezeigt.
const char WIFI_CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>bTn Wecker – WiFi Einrichtung</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#e0e0e0;
       min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
  .card{background:#16213e;border-radius:12px;padding:28px 24px;
        width:100%;max-width:380px;box-shadow:0 8px 32px rgba(0,0,0,.4)}
  h1{font-size:1.3rem;color:#4fc3f7;text-align:center;margin-bottom:4px}
  .sub{font-size:.8rem;color:#78909c;text-align:center;margin-bottom:24px}
  label{font-size:.85rem;color:#b0bec5;display:block;margin-bottom:4px;margin-top:14px}
  input[type=text],input[type=password]{
    width:100%;padding:10px 12px;border-radius:7px;border:1.5px solid #37474f;
    background:#0f3460;color:#e0e0e0;font-size:1rem;outline:none;transition:border .2s}
  input:focus{border-color:#4fc3f7}
  .pw-wrap{position:relative}
  .pw-wrap input{padding-right:44px}
  .eye{position:absolute;right:10px;top:50%;transform:translateY(-50%);
       background:none;border:none;color:#78909c;cursor:pointer;font-size:1.1rem;
       padding:4px;line-height:1}
  .eye:hover{color:#4fc3f7}
  .hint{font-size:.75rem;color:#546e7a;margin-top:3px}
  .err{background:#b71c1c;color:#fff;border-radius:7px;padding:10px 14px;
       font-size:.85rem;margin-top:16px;display:none}
  button[type=submit]{
    width:100%;margin-top:22px;padding:12px;border-radius:7px;
    background:linear-gradient(135deg,#0077b6,#4fc3f7);
    color:#fff;font-size:1rem;font-weight:bold;border:none;
    cursor:pointer;transition:opacity .2s}
  button[type=submit]:hover{opacity:.88}
  .footer{font-size:.72rem;color:#37474f;text-align:center;margin-top:18px}
</style>
</head>
<body>
<div class="card">
  <h1>&#x1F4F6; bTn Wecker</h1>
  <div class="sub">WiFi-Einrichtung</div>

  <form id="frm" method="POST" action="/save" onsubmit="return validate()">
    <label for="ssid">WLAN-Name (SSID)</label>
    <input type="text" id="ssid" name="ssid" maxlength="32"
           placeholder="Ihr WLAN-Name" autocomplete="off" autocorrect="off"
           autocapitalize="none" spellcheck="false" required>
    <div class="hint">1 – 32 Zeichen</div>

    <label for="psk">Passwort</label>
    <div class="pw-wrap">
      <input type="password" id="psk" name="psk" maxlength="63"
             placeholder="Leer = offenes Netz" autocomplete="new-password">
      <button type="button" class="eye" onclick="togglePw()" title="Passwort anzeigen">&#x1F441;</button>
    </div>
    <div class="hint">Leer lassen f&uuml;r offene Netzwerke &bull; sonst 8 – 63 Zeichen</div>

    <div class="err" id="errBox"></div>

    <button type="submit">Speichern &amp; Neu starten</button>
  </form>

  <div class="footer">bTn_Alarm_)rawliteral" FW_VERSION R"rawliteral( &nbsp;&bull;&nbsp; 192.168.4.1</div>
</div>

<script>
function togglePw(){
  var f=document.getElementById('psk');
  f.type=(f.type==='password')?'text':'password';
}
function showErr(msg){
  var b=document.getElementById('errBox');
  b.textContent=msg; b.style.display='block';
  b.scrollIntoView({behavior:'smooth',block:'nearest'});
}
function validate(){
  var ssid=document.getElementById('ssid').value.trim();
  var psk =document.getElementById('psk').value;
  if(ssid.length<1||ssid.length>32){
    showErr('SSID ung\u00fcltig: 1\u201332 Zeichen erforderlich.'); return false;
  }
  if(psk.length>0&&psk.length<8){
    showErr('Passwort ung\u00fcltig: Leer lassen oder 8\u201363 Zeichen.'); return false;
  }
  if(psk.length>63){
    showErr('Passwort zu lang (max. 63 Zeichen).'); return false;
  }
  document.getElementById('errBox').style.display='none';
  return true;
}
// URL-Parameter ?err= als Fehlermeldung anzeigen
(function(){
  var p=new URLSearchParams(window.location.search);
  if(p.get('err')) showErr(decodeURIComponent(p.get('err')));
})();
</script>
</body>
</html>
)rawliteral";

// ── Erfolgsseite ─────────────────────────────────────────────
const char WIFI_SUCCESS_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>bTn Wecker – Gespeichert</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#e0e0e0;
       min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
  .card{background:#16213e;border-radius:12px;padding:28px 24px;
        width:100%;max-width:380px;text-align:center;box-shadow:0 8px 32px rgba(0,0,0,.4)}
  .icon{font-size:3rem;margin-bottom:12px}
  h1{font-size:1.3rem;color:#66bb6a;margin-bottom:8px}
  p{font-size:.9rem;color:#b0bec5;line-height:1.5}
  .countdown{font-size:2rem;font-weight:bold;color:#4fc3f7;margin:18px 0 4px}
  .sub{font-size:.78rem;color:#546e7a}
</style>
</head>
<body>
<div class="card">
  <div class="icon">&#x2705;</div>
  <h1>Gespeichert!</h1>
  <p>Die WLAN-Zugangsdaten wurden im Flash gespeichert.</p>
  <div class="countdown" id="ct">3</div>
  <div class="sub">Sekunden bis zum Neustart</div>
</div>
<script>
var n=3;
var t=setInterval(function(){
  n--;document.getElementById('ct').textContent=n;
  if(n<=0){clearInterval(t);}
},1000);
</script>
</body>
</html>
)rawliteral";

// ── Fehlerseite (dynamisch, gibt String zurück) ───────────────
// Leitet zurück zur Konfigurationsseite mit URL-kodierter
// Fehlermeldung, die dort per JS angezeigt wird.
inline String wifiErrorRedirect(const char* msg) {
  String url = "/?err=";
  // Einfache URL-Kodierung: Leerzeichen → %20, Umlaute bleiben (UTF-8 Browser-kompatibel)
  for (const char* p = msg; *p; ++p) {
    if (*p == ' ') url += "%20";
    else           url += *p;
  }
  String page = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta http-equiv='refresh' content='0;url=");
  page += url;
  page += F("'></head><body></body></html>");
  return page;
}
