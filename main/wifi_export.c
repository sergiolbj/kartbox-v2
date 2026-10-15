/*
 * wifi_export.c - ver wifi_export.h pro contexto.
 */
#include "wifi_export.h"
#include "config.h"
#include "app_events.h"
#include "kb_pure.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "track_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "settings.h"
#include "gps.h"
#include "ui.h"
#include "sd_logger.h"

static const char *TAG = "wifi_export";

static bool        s_active = false;
static bool        s_wifi_inited = false;
static httpd_handle_t s_httpd = NULL;
static char         s_ssid[32];
static char         s_password[64] = WIFI_AP_PASSWORD_DEFAULT;
static int64_t      s_last_activity_us = 0;
static esp_timer_handle_t s_idle_timer = NULL;

static wifi_export_mode_t s_mode = WIFI_EXPORT_MODE_AP;

/* Estado do modo cliente (STA) - ver wifi_sta_event_handler(). */
/* Literal em vez de BIT0/BIT1 (esp_bit_defs.h) de proposito - nao vale a
 * pena puxar mais um header so por isso, e o valor exato nao importa. */
#define WIFI_STA_CONNECTED_BIT      (1 << 0)
#define WIFI_STA_FAIL_BIT           (1 << 1)
#define WIFI_SCAN_DONE_BIT          (1 << 2)
#define WIFI_STA_STARTED_BIT        (1 << 3)
#define WIFI_STA_MAX_RETRY          (5)
#define WIFI_STA_CONNECT_TIMEOUT_MS (15000)
#define WIFI_SCAN_TIMEOUT_MS        (8000)
/* esp_wifi_start() nesse board e' assincrono (RPC via SDIO pro C6). A
 * interface STA so esta' de fato pronta quando WIFI_EVENT_STA_START
 * chega - por isso esperamos esse bit antes de esp_wifi_scan_start(),
 * que exige a interface JA' no ar (senao volta ESP_ERR_WIFI_STATE ou
 * dispara um scan vazio, number=0). Em chip nativo a janela e' de us e
 * passava por sorte; no hosted e' de ms e perdia sempre. */
#define WIFI_STA_START_TIMEOUT_MS   (3000)
/* Dwell por canal no active scan. No hosted o default (0) as vezes nao
 * captura AP nenhum - fixamos min/max explicitos. */
#define WIFI_SCAN_DWELL_MIN_MS      (120)
#define WIFI_SCAN_DWELL_MAX_MS      (300)
/* Retry do esp_wifi_scan_start quando o slave ainda esta num estado
 * transitorio ("connecting") e recusa o scan com ESP_ERR_WIFI_STATE. */
#define WIFI_SCAN_START_RETRIES     (4)
#define WIFI_SCAN_START_RETRY_MS    (250)

static char              s_sta_ssid[WIFI_SCAN_SSID_MAX];
static char              s_sta_password[64];
static char              s_sta_ip[16] = "";
static volatile wifi_sta_state_t s_sta_state = WIFI_STA_STATE_IDLE;
static int                s_sta_retry_count = 0;
static EventGroupHandle_t s_wifi_event_group = NULL;

/* Resultado do scan, capturado DENTRO do handler de evento (ver
 * WIFI_EVENT_SCAN_DONE em wifi_sta_event_handler), nao depois em
 * wifi_export_scan(). Nesse board o radio e' remoto (esp_hosted via SDIO
 * pro C6) - mesmo esperando o evento real de "scan terminou" (que ja
 * corrigiu o scan voltando 0 por nao esperar nada), esp_wifi_scan_get_ap_
 * num()/get_ap_records() chamados de volta em wifi_export_scan() (outra
 * task, minimos ms depois) ainda voltavam 0 - o log mostra o coprocessador
 * derrubando a STA ("wifi station stopped") logo em seguida ao scan, e
 * essa janela e' pequena o suficiente pra perder a corrida contra
 * qualquer round-trip de RPC extra. Fix: le os resultados JA' dentro do
 * handler, o mais perto possivel do evento em si (mesmo instante,
 * garantido pelo esp_event antes de qualquer teardown seguinte). */
static wifi_ap_record_t   s_scan_records[WIFI_SCAN_MAX_RESULTS];
static uint16_t           s_scan_got = 0;

/* True so' durante wifi_export_scan(). O handler de STA_DISCONNECTED NAO
 * pode auto-reconectar enquanto escaneamos: o disconnect() defensivo do
 * scan dispara esse evento e, sem essa guarda, o handler chamaria
 * esp_wifi_connect() de volta (na config vazia), reabrindo justamente o
 * estado "connecting" que bloqueia o scan (ESP_ERR_WIFI_STATE). */
static volatile bool      s_scanning = false;

static void touch_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* ---------------------------------------------------------------------
 * Helpers de entrada/saida web
 * --------------------------------------------------------------------- */

/* Decodifica %XX e '+' in-place (query string). */
static void url_decode_inplace(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(h, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* httpd_query_key_value + URL-decode (a lib devolve o valor cru: "a%20b"). */
static esp_err_t query_param(const char *query, const char *key, char *out, size_t len)
{
    esp_err_t e = httpd_query_key_value(query, key, out, len);
    if (e == ESP_OK) url_decode_inplace(out);
    return e;
}

/* Nome de arquivo de sessao/upload aceitavel: sem separador de caminho,
 * sem "..", sem aspas e sem controle (CR/LF viraria injecao de header no
 * Content-Disposition depois do decode). */
static bool web_filename_ok(const char *n)
{
    if (!n || !n[0] || strstr(n, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)n; *p; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '/' || *p == '\\' || *p == '"') return false;
    }
    return true;
}

/* Envia texto com escape HTML (texto e atributos entre aspas). */
static void send_html_esc(httpd_req_t *req, const char *s)
{
    char buf[96]; size_t n = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        }
        size_t add = rep ? strlen(rep) : 1;
        if (n + add >= sizeof(buf)) { buf[n] = 0; httpd_resp_sendstr_chunk(req, buf); n = 0; }
        if (rep) { memcpy(buf + n, rep, add); n += add; } else buf[n++] = *s;
    }
    if (n) { buf[n] = 0; httpd_resp_sendstr_chunk(req, buf); }
}

/* Envia texto percent-encoded (pra href/query). */
static void send_url_enc(httpd_req_t *req, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[96]; size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool safe = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t add = safe ? 1 : 3;
        if (n + add >= sizeof(buf)) { buf[n] = 0; httpd_resp_sendstr_chunk(req, buf); n = 0; }
        if (safe) buf[n++] = (char)*p;
        else { buf[n++] = '%'; buf[n++] = hex[*p >> 4]; buf[n++] = hex[*p & 15]; }
    }
    if (n) { buf[n] = 0; httpd_resp_sendstr_chunk(req, buf); }
}

/* ---------------------------------------------------------------------
 * HTTP - lista sessoes + download. Path traversal corrigido aqui: so
 * aceita basename, rejeita "..". O codigo da v1 (nunca usado em
 * producao) montava o path direto do parametro da URL sem checar nada.
 * --------------------------------------------------------------------- */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=\"pt-br\"><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>KartBox - sessoes</title>"
        "<style>"
        ":root{--bg:#0d0f0d;--surface:#141714;--line:#262626;--text:#f5f8f5;"
        "--muted:#7a847a;--green:#3ee07a;--gold:#ffd700;--cyan:#3ec6e0;}"
        "*{box-sizing:border-box}"
        "body{margin:0 auto;padding:20px;max-width:720px;background:var(--bg);color:var(--text);"
        "font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;line-height:1.5}"
        "h1{margin:0 0 16px;font-size:22px;color:var(--green);border-bottom:2px solid var(--line);padding-bottom:10px}"
        "h3{color:var(--muted);font-size:13px;text-transform:uppercase;letter-spacing:1px;margin:26px 0 8px}"
        "a{color:var(--cyan);text-decoration:none}a:hover{text-decoration:underline}"
        "button,.btn{background:var(--surface);color:var(--text);border:1px solid var(--line);"
        "border-radius:8px;padding:8px 12px;font-size:14px;cursor:pointer;min-height:38px}"
        "button:active{background:#1d211d}"
        "input[type=file]{color:var(--muted);font-size:13px;margin-right:6px}"
        "ul{list-style:none;padding:0;margin:16px 0}"
        "li{background:var(--surface);border:1px solid var(--line);border-left:3px solid var(--green);"
        "border-radius:8px;padding:12px 14px;margin-bottom:10px;display:flex;flex-wrap:wrap;gap:8px;align-items:center}"
        "li b{flex-basis:100%;color:var(--gold);font-weight:600;margin-bottom:2px}"
        ".meta{flex-basis:100%;color:var(--muted);font-size:12px;margin-bottom:6px}"
        "button.del{border-color:#ff5a5a;color:#ff5a5a}button.del:active{background:#2a1414}"
        "li a{border:1px solid var(--line);border-radius:8px;padding:8px 12px;min-height:38px;display:inline-flex;align-items:center}"
        "#upstatus{color:var(--muted);font-size:13px;margin-left:6px}"
        "</style></head><body>"
        "<h1>KartBox - sessoes</h1>"
        /* atalhos em cards clicaveis (nao links soltos) - mesmo padrao
         * visual dos <li> de sessao, com acento lateral */
        "<div style=\"display:flex;gap:10px;margin:14px 0;flex-wrap:wrap\">"
        "<a href=\"/editor\" style=\"flex:1;min-width:220px;background:var(--surface);"
        "border:1px solid var(--line);border-left:3px solid var(--cyan);border-radius:8px;"
        "padding:14px 16px;color:var(--text);font-size:15px;text-decoration:none\">"
        "&#128205; <b>Editor de pista</b><br>"
        "<small style=\"color:var(--muted)\">marcar linha de chegada e setores no mapa</small></a>"
        "<a href=\"/evolucao\" style=\"flex:1;min-width:220px;background:var(--surface);"
        "border:1px solid var(--line);border-left:3px solid var(--green);border-radius:8px;"
        "padding:14px 16px;color:var(--text);font-size:15px;text-decoration:none\">"
        "&#128200; <b>Evolucao entre sessoes</b><br>"
        "<small style=\"color:var(--muted)\">best, media e consistencia por dia, pista a pista</small></a>"
        "<a href=\"/arquivos\" style=\"flex:1;min-width:220px;background:var(--surface);"
        "border:1px solid var(--line);border-left:3px solid var(--gold);border-radius:8px;"
        "padding:14px 16px;color:var(--text);font-size:15px;text-decoration:none\">"
        "&#128193; <b>Arquivos do SD</b><br>"
        "<small style=\"color:var(--muted)\">navegar pastas, baixar, enviar e apagar arquivos</small></a>"
        "</div>"
        /* Upload pelo browser - pedido do usuario. Le o arquivo local
         * como bytes crus (arrayBuffer) e manda via fetch() com o corpo
         * sendo o proprio arquivo (sem multipart/form-data - nao vale a
         * pena implementar um parser multipart no firmware so pra isso,
         * e esse jeito e' tao suportado quanto em qualquer navegador
         * atual). Nome do arquivo vai na query string; upload_post_handler
         * grava direto na pasta de sessoes com esse nome. */
        "<h3>Enviar arquivo pro cartao</h3>"
        "<p style=\"color:var(--muted);font-size:13px;margin:4px 0 8px\">"
        "destino automatico: <b>.csv</b> &rarr; sessions &middot; <b>.trk</b> &rarr; tracks &middot; "
        "qualquer outro (ex: backup de config) &rarr; raiz do cartao</p>"
        "<input type=\"file\" id=\"upfile\"> "
        "<button onclick=\"kbUpload()\">Enviar</button> "
        "<span id=\"upstatus\"></span>"
        "<script>"
        "function kbUpload(){"
        "var inp=document.getElementById('upfile');"
        "var st=document.getElementById('upstatus');"
        "if(!inp.files.length){st.textContent='Escolha um arquivo primeiro';return;}"
        "var f=inp.files[0];"
        "st.textContent='Enviando...';"
        "f.arrayBuffer().then(function(buf){"
        "return fetch('/upload?file='+encodeURIComponent(f.name),{method:'POST',body:buf});"
        "}).then(function(r){return r.text().then(function(t){"
        "if(r.ok){st.textContent=t;setTimeout(function(){location.reload();},1400);}"
        "else{st.textContent='Erro ao enviar ('+r.status+'): '+t;}"
        "});"
        "}).catch(function(){st.textContent='Erro de rede';});"
        "}"
        "</script>"
        "<ul>");

    DIR *dir = opendir(SD_SESSIONS_DIR);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len < 5 || strcmp(de->d_name + len - 4, ".csv") != 0) continue;
            /* Cada sessao: link do CSV cru + botoes que convertem no
             * proprio navegador (ver script kbExp abaixo) pra GPX / VBO /
             * RaceChrono, sem inchar o firmware com os conversores. O nome
             * do arquivo entra 5x - mandado em pedacos (sendstr_chunk) em
             * vez de um snprintf num buffer fixo, pra nao arriscar
             * truncamento (nome de sessao pode ter qualquer tamanho). */
            /* Nome do arquivo vem do SD (pode ter aspas, <, ' ...): texto/atributo
             * com escape HTML; href com percent-encode; botoes leem o nome de
             * data-f (nunca interpolado dentro de string JS). */
            httpd_resp_sendstr_chunk(req, "<li><b>");
            send_html_esc(req, de->d_name);
            /* <small> com data-file: preenchido no navegador (kbMeta) com a
             * data legivel e a pista, derivadas do nome do arquivo. */
            httpd_resp_sendstr_chunk(req, "</b><small class=\"meta\" data-file=\"");
            send_html_esc(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "\"></small> <a href=\"/download?file=");
            send_url_enc(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "\">CSV</a> <span data-f=\"");
            send_html_esc(req, de->d_name);
            httpd_resp_sendstr_chunk(req,
                "\"><button onclick=\"kbExp(this.parentNode.dataset.f,'gpx')\">GPX</button> "
                "<button onclick=\"kbExp(this.parentNode.dataset.f,'vbo')\">VBO</button> "
                "<button onclick=\"kbExp(this.parentNode.dataset.f,'rc')\">RaceChrono</button> "
                "<a href=\"/analise?file=");
            send_url_enc(req, de->d_name);
            httpd_resp_sendstr_chunk(req,
                "\">Analise</a> <a href=\"/relatorio?file=");
            send_url_enc(req, de->d_name);
            httpd_resp_sendstr_chunk(req,
                "\">Relatorio</a> <button class=\"del\" onclick=\"kbDel(this.parentNode.dataset.f)\">Excluir</button></span></li>");
        }
        closedir(dir);
    }

    httpd_resp_sendstr_chunk(req, "</ul>");

    /* ------------------------------------------------------------------
     * Conversores de sessao, 100% no navegador. Baixa o CSV cru da sessao
     * (/download), converte e dispara o download do arquivo no formato
     * escolhido. Formatos:
     *   GPX  - universal; Harry's LapTimer, Google Earth, etc.
     *   VBO  - RaceLogic VBOX; importado nativamente pelo RaceChrono e
     *          Circuit Tools (lat/long em MINUTOS, Oeste positivo).
     *   RC   - CSV com as colunas de export do RaceChrono (planilha).
     * Timestamp absoluto derivado do nome do arquivo (datetime do GPS na
     * criacao da sessao) + t_ms de cada linha.
     * ------------------------------------------------------------------ */
    httpd_resp_sendstr_chunk(req,
        "<script>\n"
        "function kbPad(n,l){n=''+n;while(n.length<l)n='0'+n;return n;}\n"
        "function kbStart(f){var m=f.match(/(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})/);"
        "if(!m)return new Date(0);return new Date(Date.UTC(+m[1],+m[2]-1,+m[3],+m[4],+m[5],+m[6]));}\n"
        "function kbRows(csv){var L=csv.trim().split(/\\r?\\n/);var r=[];"
        "for(var i=1;i<L.length;i++){var c=L[i].split(',');if(c.length<6)continue;"
        "r.push({t:+c[0],lat:+c[1],lon:+c[2],spd:+c[3],hdg:+c[4],sat:+c[5]|0,lap:+c[6]||0});}return r;}\n"
        "function kbGPX(csv,f){var r=kbRows(csv),st=kbStart(f);"
        "var o='<?xml version=\"1.0\" encoding=\"UTF-8\"?>\\n<gpx version=\"1.1\" creator=\"KartBox\" xmlns=\"http://www.topografix.com/GPX/1/1\">\\n<trk><name>'+f+'</name><trkseg>\\n';"
        "for(var i=0;i<r.length;i++){var t=new Date(st.getTime()+r[i].t).toISOString();"
        "o+='<trkpt lat=\"'+r[i].lat.toFixed(6)+'\" lon=\"'+r[i].lon.toFixed(6)+'\"><time>'+t+'</time>"
        "<extensions><speed>'+(r[i].spd/3.6).toFixed(2)+'</speed></extensions></trkpt>\\n';}"
        "o+='</trkseg></trk></gpx>\\n';return o;}\n"
        "function kbVBO(csv,f){var r=kbRows(csv),st=kbStart(f);"
        "var hd=kbPad(st.getUTCDate(),2)+'/'+kbPad(st.getUTCMonth()+1,2)+'/'+st.getUTCFullYear();"
        "var ht=kbPad(st.getUTCHours(),2)+':'+kbPad(st.getUTCMinutes(),2)+':'+kbPad(st.getUTCSeconds(),2);"
        "var o='File created on '+hd+' at '+ht+'\\n\\n[header]\\nsatellites\\ntime\\nlatitude\\nlongitude\\nvelocity kmh\\nheading\\n\\n[column names]\\nsats time lat long velocity heading\\n\\n[data]\\n';"
        "var base=st.getUTCHours()*3600+st.getUTCMinutes()*60+st.getUTCSeconds();"
        "for(var i=0;i<r.length;i++){var sec=base+r[i].t/1000;"
        "var hh=Math.floor(sec/3600),mm=Math.floor((sec%3600)/60),ss=sec%60;"
        "var tm=kbPad(hh,2)+kbPad(mm,2)+(ss<10?'0':'')+ss.toFixed(2);"
        "var latm=(r[i].lat*60).toFixed(5),lonm=(-r[i].lon*60).toFixed(5);"
        "o+=kbPad(r[i].sat,3)+' '+tm+' '+(r[i].lat<0?'':'+')+latm+' '+((-r[i].lon)<0?'':'+')+lonm+' '+r[i].spd.toFixed(2)+' '+r[i].hdg.toFixed(2)+'\\n';}"
        "return o;}\n"
        "function kbRC(csv,f){var r=kbRows(csv);"
        "var o='Session title,'+f+'\\nLap #,Timestamp (s),Locked satellites,Latitude (deg),Longitude (deg),Speed (kph),Altitude (m),Bearing (deg)\\n';"
        "for(var i=0;i<r.length;i++){o+=r[i].lap+','+(r[i].t/1000).toFixed(3)+','+r[i].sat+','+r[i].lat.toFixed(6)+','+r[i].lon.toFixed(6)+','+r[i].spd.toFixed(2)+',0,'+r[i].hdg.toFixed(1)+'\\n';}return o;}\n"
        "function kbExp(f,fmt){fetch('/download?file='+encodeURIComponent(f)).then(function(x){return x.text();}).then(function(csv){"
        "var out,name,mime,b=f.replace(/\\.csv$/i,'');"
        "if(fmt=='gpx'){out=kbGPX(csv,f);name=b+'.gpx';mime='application/gpx+xml';}"
        "else if(fmt=='vbo'){out=kbVBO(csv,f);name=b+'.vbo';mime='text/plain';}"
        "else{out=kbRC(csv,f);name=b+'_racechrono.csv';mime='text/csv';}"
        "var bl=new Blob([out],{type:mime});var a=document.createElement('a');"
        "a.href=URL.createObjectURL(bl);a.download=name;a.click();"
        "}).catch(function(){alert('Falha ao converter '+f);});}\n"
        "function kbDel(f){if(!confirm('Apagar a sessao '+f+'? Nao tem como desfazer.'))return;"
        "fetch('/delete?file='+encodeURIComponent(f),{method:'POST'}).then(function(r){"
        "if(r.ok)location.reload();else alert('Falha ao apagar ('+r.status+')');"
        "}).catch(function(){alert('Erro de rede ao apagar');});}\n"
        "function kbMeta(){var el=document.querySelectorAll('.meta');"
        "for(var i=0;i<el.length;i++){var f=el[i].getAttribute('data-file');"
        "var m=f.match(/(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})/);var p=[];"
        "if(m)p.push(m[3]+'/'+m[2]+'/'+m[1]+' '+m[4]+':'+m[5]);"
        "var tk=f.replace(/\\.csv$/i,'');tk=m?tk.slice(m.index+15):'';"
        "if(tk&&tk.charAt(0)=='_')tk=tk.slice(1);"
        "if(tk)p.push('Pista: '+tk.replace(/_/g,' '));"
        "el[i].textContent=p.length?p.join(' - '):'';}}\n"
        "kbMeta();\n"
        "function kbLaps(){var el=document.querySelectorAll('.meta'),i=0;"
        "function nx(){if(i>=el.length)return;var e=el[i++],f=e.getAttribute('data-file');"
        "fetch('/api/laps?file='+encodeURIComponent(f)).then(function(r){return r.text();}).then(function(t){"
        "t=t.trim();var n=t?t.split(/\\n/).length:0;"
        "e.textContent+=(e.textContent?' - ':'')+n+(n==1?' volta':' voltas');}).catch(function(){}).then(nx);}nx();}\n"
        "kbLaps();\n"
        "</script>");

    /* ------------------------------------------------------------------
     * OTA - atualizar o firmware pelo navegador. O usuario escolhe o
     * .bin do app gerado pelo build e o kbOta() manda os bytes crus
     * num POST /ota (mesmo estilo sem-multipart do upload de sessao).
     * Validacao/gravacao/troca de boot em ota_post_handler.
     * ------------------------------------------------------------------ */
    /* (Secao A-GPS removida da pagina a pedido do usuario - os endpoints
     * POST /agps e GET /agps_dl continuam vivos pra uso direto/futuro,
     * ex: injecao automatica ao conectar em STA.) */
    httpd_resp_sendstr_chunk(req,
        "<h3>Atualizar firmware (OTA)</h3>"
        "<p style=\"color:var(--muted);font-size:13px\">Versao atual: ");
    httpd_resp_sendstr_chunk(req, esp_app_get_description()->version);
    httpd_resp_sendstr_chunk(req,
        "</p>"
        "<input type=\"file\" id=\"otafile\" accept=\".bin\"> "
        "<button onclick=\"kbOta()\">Atualizar</button> "
        "<span id=\"otastatus\"></span>"
        "<script>"
        /* XHR (nao fetch): fetch nao expoe progresso de UPLOAD, e sem
         * porcentagem o usuario ficava as cegas por ~1 min achando que
         * travou (a tela do aparelho piscando durante a gravacao da
         * flash piorava a impressao). */
        "function kbOta(){"
        "var inp=document.getElementById('otafile');"
        "var st=document.getElementById('otastatus');"
        "if(!inp.files.length){st.textContent='Escolha o .bin do build primeiro';return;}"
        "if(!confirm('Atualizar o firmware? O KartBox mostra ATUALIZANDO na tela e reinicia sozinho ao terminar. NAO desligue durante o processo.'))return;"
        "var x=new XMLHttpRequest();"
        "x.open('POST','/ota');"
        "x.timeout=180000;"
        "x.upload.onprogress=function(e){"
        "if(e.lengthComputable){st.textContent='Enviando... '+Math.round(e.loaded*100/e.total)+'%';}"
        "};"
        "x.onload=function(){"
        "if(x.status==200){st.textContent='OK! Gravado e validado - reiniciando. Aguarde ~20s e recarregue esta pagina: a \\'Versao atual\\' deve mudar.';}"
        "else{st.textContent='Falhou: '+x.responseText;}"
        "};"
        "x.onerror=x.ontimeout=function(){"
        "st.textContent='Conexao caiu no envio. Se chegou em 100%, o update pode ter concluido mesmo assim - aguarde o reinicio e confira a Versao atual. Senao, tente de novo.';"
        "};"
        "x.send(inp.files[0]);"
        "}"
        "</script>");

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    touch_activity();

    char query[160];
    char filename[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "file", filename, sizeof(filename));
    }

    /* SO o nome do arquivo. Rejeita qualquer coisa que tente sair da
     * pasta de sessoes (ou injetar header via CR/LF/aspas). */
    const char *base = filename;
    if (!web_filename_ok(base)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nome de arquivo invalido");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, base);

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char disp_hdr[96];
    snprintf(disp_hdr, sizeof(disp_hdr), "attachment; filename=\"%s\"", base);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* 8MB - generoso pra qualquer sessao CSV real (sessao de 1h a 10Hz fica
 * bem abaixo disso) sem deixar a rota aceitar upload arbitrariamente
 * grande e travar o SD/RAM. */
#define WIFI_UPLOAD_MAX_BYTES (8 * 1024 * 1024)

/* POST /upload?file=NOME - grava o CORPO CRU da requisicao (nao
 * multipart/form-data - ver comentario no HTML de index_get_handler
 * sobre o motivo) direto num arquivo na pasta de sessoes. Mesma
 * sanitizacao de nome do download (so basename, rejeita ".."). Pedido do
 * usuario: dar a opcao de enviar arquivo pelo browser, nao so baixar. */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    touch_activity();

    char query[160];
    char filename[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "file", filename, sizeof(filename));
    }
    const char *base = filename;
    if (!web_filename_ok(base)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nome de arquivo invalido");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > WIFI_UPLOAD_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tamanho invalido (max 8MB)");
        return ESP_FAIL;
    }

    /* Destino por extensao (correcao de campo: TUDO caia em sessions/,
     * inclusive o editor.html que precisa ficar na RAIZ do cartao):
     *   .csv -> sessions/ (dado de sessao)
     *   .trk -> tracks/   (config de pista)
     *   resto -> raiz     (editor.html, backup de config, etc) */
    const char *ext = strrchr(base, '.');
    const char *dest_dir = SD_MOUNT_POINT;
    if (ext && strcasecmp(ext, ".csv") == 0) dest_dir = SD_SESSIONS_DIR;
    else if (ext && strcasecmp(ext, ".trk") == 0) dest_dir = SD_TRACKS_DIR;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", dest_dir, base);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload: falha ao criar %s no SD", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao criar arquivo no SD");
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            fclose(f);
            remove(path); /* upload incompleto - nao deixa lixo pela metade no cartao */
            ESP_LOGW(TAG, "upload: corpo incompleto pra %s", base);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao receber corpo");
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            fclose(f);
            remove(path); /* cartao cheio/erro de escrita - nao deixa arquivo truncado */
            ESP_LOGE(TAG, "upload: falha de escrita em %s (SD cheio?)", path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao gravar no SD (cheio?)");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    if (fclose(f) != 0) {
        remove(path);
        ESP_LOGE(TAG, "upload: fclose falhou em %s", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao finalizar arquivo no SD");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload recebido: %s (%d bytes)", path, (int)req->content_len);
    /* resposta diz ONDE salvou - a pagina mostra esse texto */
    char msg[128];
    snprintf(msg, sizeof(msg), "ok - salvo em %s", path);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * OTA - POST /ota, corpo = .bin do app cru (sem multipart, igual ao
 * /upload). Grava na particao inativa (ota_0 <-> ota_1) e so troca o
 * boot DEPOIS que esp_ota_end() validou a imagem inteira - upload
 * corrompido/arquivo errado nunca vira particao de boot. Com
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, o firmware novo ainda precisa
 * confirmar o proprio boot (esp_ota_mark_app_valid em main.c) - se
 * crashar antes disso, o bootloader volta sozinho pra versao anterior.
 * --------------------------------------------------------------------- */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    touch_activity();

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        /* acontece se ainda estiver rodando da tabela antiga (factory
         * unica) - a migracao pra ota_0/ota_1 exige um ultimo flash por
         * cabo (ver comentario em partitions.csv). */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "sem particao OTA - flasheie uma vez por cabo com a partition table nova");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tamanho invalido pro firmware");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: recebendo %u bytes pra particao %s",
             (unsigned)req->content_len, part->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin falhou (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao preparar particao");
        return ESP_FAIL;
    }

    /* Overlay estatico na tela do aparelho - avisa o piloto e congela a
     * UI (menos redesenho disputando banda com a gravacao da flash =
     * menos underrun/piscada no painel DSI). */
    ui_show_ota_progress(true);

    char buf[1024];
    int remaining = (int)req->content_len;
    int since_yield = 0;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            esp_ota_abort(ota);
            ui_show_ota_progress(false);
            ESP_LOGW(TAG, "OTA: corpo incompleto (faltavam %d bytes)", remaining);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrompido");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            esp_ota_abort(ota);
            ui_show_ota_progress(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha de escrita na flash");
            return ESP_FAIL;
        }
        remaining -= r;
        /* Respiro a cada ~32KB gravados: escrita de flash em rajada
         * continua estrangula WiFi (esp_hosted) e display - era o que
         * derrubava a conexao no meio do upload sem feedback nenhum. */
        since_yield += r;
        if (since_yield >= 32 * 1024) {
            since_yield = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    err = esp_ota_end(ota); /* valida magic + checksum/sha da imagem inteira */
    if (err != ESP_OK) {
        ui_show_ota_progress(false);
        ESP_LOGE(TAG, "esp_ota_end falhou (%s) - imagem invalida", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "imagem invalida - envie o .bin do APP gerado pelo build");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ui_show_ota_progress(false);
        ESP_LOGE(TAG, "esp_ota_set_boot_partition falhou (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao marcar particao de boot");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "ok");
    ESP_LOGI(TAG, "OTA validado em %s - reiniciando em 1s", part->label);
    vTaskDelay(pdMS_TO_TICKS(1000)); /* deixa a resposta HTTP sair antes do reset */
    esp_restart();
    return ESP_OK; /* nunca chega */
}

/* ---------------------------------------------------------------------
 * GET /analise?file=X - pagina de analise velocidade x distancia.
 * HTML autocontido; o proprio JS baixa o CSV cru via /download, separa
 * as voltas, integra a distancia (projecao plana, mesma do mapa) e
 * plota 2 voltas selecionaveis num <canvas>. Nada e' processado no
 * firmware - so servimos a pagina e o CSV que ja existiam.
 * --------------------------------------------------------------------- */
static esp_err_t analysis_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>KartBox - analise</title><style>"
        "body{margin:0 auto;padding:16px;max-width:960px;background:#0d0f0d;color:#f5f8f5;"
        "font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif}"
        "h1{font-size:18px;color:#3ee07a;border-bottom:2px solid #262626;padding-bottom:8px}"
        "a{color:#3ec6e0;text-decoration:none}"
        "select{background:#141714;color:#f5f8f5;border:1px solid #262626;border-radius:8px;"
        "padding:8px;font-size:14px;margin:0 8px 12px 4px}"
        "canvas{width:100%;background:#141714;border:1px solid #262626;border-radius:8px}"
        "#err{color:#ff5a5a}"
        ".lg{font-size:13px;color:#7a847a;margin:8px 0}"
        "button{background:#141714;color:#f5f8f5;border:1px solid #262626;border-radius:8px;"
        "padding:8px 14px;font-size:14px;cursor:pointer}"
        /* relatorio imprimivel: papel branco, sem controles de navegacao */
        "@media print{body{background:#fff;color:#000;max-width:100%}"
        "h1{color:#000;border-color:#999}h3{color:#333}"
        ".noprint{display:none!important}"
        "#stats{color:#000!important}"
        "canvas{border:1px solid #999}}"
        "</style></head><body>"
        "<p class=\"noprint\"><a href=\"/\">&larr; sessoes</a> &middot; <a id=\"rl\" href=\"#\">relatorio completo da sessao</a></p>"
        "<h1 id=\"hd\">Analise</h1>"
        "<div class=\"noprint\">Volta A <select id=\"selA\"></select> "
        "Volta B <select id=\"selB\"></select> "
        "<button onclick=\"window.print()\" style=\"float:right\">Imprimir relatorio</button> "
        "<button onclick=\"shareCard()\" style=\"float:right;margin-right:8px\">Compartilhar resumo</button></div>"
        "<div id=\"stats\" class=\"lg\" style=\"font-size:15px;color:#f5f8f5\"></div>"
        "<canvas id=\"cv\" width=\"1200\" height=\"460\"></canvas>"
        "<canvas id=\"dc\" width=\"1200\" height=\"280\" style=\"margin-top:10px\"></canvas>"
        "<canvas id=\"mv\" width=\"1200\" height=\"380\" style=\"margin-top:10px\"></canvas>"
        "<p class=\"lg noprint\">passe o dedo/mouse no grafico: o ponto correspondente acende no mapa</p>"
        "<h3 style=\"color:#7a847a;font-size:13px;text-transform:uppercase;letter-spacing:1px\">Insights da sessao</h3>"
        "<ul id=\"gins\" style=\"margin:4px 0;padding-left:20px;font-size:15px;line-height:1.8\"></ul>"
        "<h3 style=\"color:#7a847a;font-size:13px;text-transform:uppercase;letter-spacing:1px\">Comparativo volta A x B</h3>"
        "<ul id=\"ins\" style=\"margin:4px 0;padding-left:20px;font-size:15px;line-height:1.8\"></ul>"
        "<p class=\"lg\">eixo X = distancia na volta (m) &middot; eixo Y = km/h &middot; "
        "<span style=\"color:#3ee07a\">verde = volta A</span> &middot; "
        "<span style=\"color:#3ec6e0\">ciano = volta B</span>. "
        "Onde o ciano cai abaixo do verde e' onde a volta B perde velocidade (freou antes / saiu pior).</p>"
        "<p id=\"err\"></p>"
        "<script>\n"
        "var laps={};\n"
        "function load(){\n"
        "var f=new URLSearchParams(location.search).get('file');\n"
        "if(!f){err.textContent='sem arquivo na URL';return;}\n"
        "hd.textContent='Analise - '+f;rl.href='/relatorio?file='+encodeURIComponent(f);\n"
        "fetch('/download?file='+encodeURIComponent(f)).then(function(r){return r.text();}).then(function(csv){\n"
        "var L=csv.trim().split(/\\r?\\n/);var prev=null;var ox=null,oy=null,okx=1;\n"
        "for(var i=1;i<L.length;i++){var c=L[i].split(',');if(c.length<8)continue;\n"
        "var lap=+c[6];if(lap<1)continue;\n"
        "var lat=+c[1],lon=+c[2],v=+c[3],t=+c[7],sat=+c[5]||0;\n"
        "if(lat==0&&lon==0)continue;\n"
        "if(ox===null){ox=lon;oy=lat;okx=111320*Math.cos(lat*Math.PI/180);}\n"
        "if(!laps[lap])laps[lap]={d:[],v:[],t:[],x:[],y:[],sat:[],last:0,time:0,vmax:0,vsum:0,vcnt:0,s1d:0,s2d:0};\n"
        "var o=laps[lap];\n"
        "if(prev&&prev.lap==lap){var kx=111320*Math.cos(lat*Math.PI/180);\n"
        "var dx=(lon-prev.lon)*kx,dy=(lat-prev.lat)*111320;\n"
        "o.last+=Math.sqrt(dx*dx+dy*dy);}\n"
        "o.d.push(o.last);o.v.push(v);o.t.push(t);o.time=t;\n"
        "o.x.push((lon-ox)*okx);o.y.push((lat-oy)*111320);o.sat.push(sat);\n"
        /* vel max/med (mesmo criterio do menu VOLTAS: media ignora <1km/h) */
        "if(v>o.vmax)o.vmax=v;if(v>1){o.vsum+=v;o.vcnt++;}\n"
        /* distancia onde S1/S2 foram cruzados: primeira linha da volta em
         * que a coluna s1_ms/s2_ms (CSV novo, 12 colunas) deixa de ser 0 */
        "if(c.length>=12){if(+c[10]>0&&!o.s1d)o.s1d=o.last;if(+c[11]>0&&!o.s2d)o.s2d=o.last;\n"
        "if(+c[10]>0)o.s1t=+c[10];if(+c[11]>0)o.s2t=+c[11];}\n"
        "prev={lat:lat,lon:lon,lap:lap};\n"
        "}\n"
        /* O MAIOR numero de volta do arquivo e' sempre uma volta EM
         * ANDAMENTO (so fecha quando o numero seguinte aparece) - e' a
         * volta de entrada no box / encerramento por botao. Descarta,
         * igual o leitor do firmware faz - senao ela aparece com tempo
         * parcial curtissimo e vira um falso "best" (visto em campo). */
        "var ks=Object.keys(laps).map(Number);\n"
        "if(ks.length)delete laps[Math.max.apply(null,ks)];\n"
        "var nums=Object.keys(laps).map(Number).sort(function(a,b){return a-b;});\n"
        "if(!nums.length){err.textContent='nenhuma volta COMPLETA nessa sessao (a ultima volta, encerrada no botao/box, nao conta)';return;}\n"
        "nums.forEach(function(n){var t=(laps[n].time/1000).toFixed(3)+'s';\n"
        "selA.add(new Option('Volta '+n+' ('+t+')',n));\n"
        "selB.add(new Option('Volta '+n+' ('+t+')',n));});\n"
        "var best=nums.reduce(function(a,b){return laps[b].time<laps[a].time?b:a;});\n"
        "selA.value=best;selB.value=nums[nums.length-1];\n"
        "sessionInsights(nums,best);\n"
        "draw();\n"
        "}).catch(function(){err.textContent='falha ao baixar o CSV';});\n"
        "}\n"
        "function draw(){\n"
        "var A=laps[selA.value],B=laps[selB.value];if(!A||!B)return;\n"
        "var ctx=cv.getContext('2d');ctx.clearRect(0,0,cv.width,cv.height);\n"
        "var maxd=Math.max(A.last,B.last)||1,maxv=0,minv=1e9;\n"
        "[A,B].forEach(function(o){o.v.forEach(function(x){if(x>maxv)maxv=x;if(x<minv)minv=x;});});\n"
        "if(maxv-minv<1)maxv=minv+1;\n"
        "var mL=56,W=cv.width-mL-12,H=cv.height-40;\n"
        "ctx.font='15px monospace';\n"
        "for(var g=0;g<=5;g++){var y=8+H-g*H/5;\n"
        "ctx.strokeStyle='#262626';ctx.beginPath();ctx.moveTo(mL,y);ctx.lineTo(mL+W,y);ctx.stroke();\n"
        "ctx.fillStyle='#7a847a';ctx.fillText(Math.round(minv+(maxv-minv)*g/5),8,y+5);}\n"
        "for(var g=0;g<=4;g++){var x=mL+g*W/4;\n"
        "ctx.fillStyle='#7a847a';ctx.fillText(Math.round(maxd*g/4)+'m',x-14,cv.height-8);}\n"
        /* legendas dos eixos */
        "ctx.fillStyle='#7a847a';\n"
        "ctx.fillText('km/h',mL+8,22);\n"
        "ctx.fillText('distancia na volta \\u2192',mL+W-190,cv.height-8);\n"
        "function plot(o,col){ctx.strokeStyle=col;ctx.lineWidth=2;ctx.beginPath();\n"
        "for(var i=0;i<o.d.length;i++){var x=mL+o.d[i]/maxd*W,y=8+H-(o.v[i]-minv)/(maxv-minv)*H;\n"
        "if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y);}ctx.stroke();}\n"
        /* tracejados verticais nos cruzamentos de setor (referencia:
         * volta A; cai pra B se A nao tiver splits) + rotulos S1/S2/S3 */
        "var s1=A.s1d||B.s1d,s2=A.s2d||B.s2d;\n"
        "function vline(d){if(!d)return 0;var x=mL+d/maxd*W;\n"
        "ctx.setLineDash([6,6]);ctx.strokeStyle='#5a5f58';ctx.lineWidth=1;\n"
        "ctx.beginPath();ctx.moveTo(x,8);ctx.lineTo(x,8+H);ctx.stroke();ctx.setLineDash([]);return x;}\n"
        "var x1=vline(s1),x2=vline(s2);\n"
        "ctx.fillStyle='#ffd700';\n"
        "if(x1&&x2){ctx.fillText('S1',(mL+x1)/2-8,28);ctx.fillText('S2',(x1+x2)/2-8,28);\n"
        "ctx.fillText('S3',(x2+mL+W)/2-8,28);}\n"
        "else if(x1){ctx.fillText('S1',(mL+x1)/2-8,28);ctx.fillText('S2',(x1+mL+W)/2-8,28);}\n"
        "plot(A,'#3ee07a');plot(B,'#3ec6e0');\n"
        /* qualidade de dados: re-desenha em vermelho translucido os
         * trechos com sinal fraco (sats<5) - dado dali e' suspeito */
        "function plotBad(o){ctx.strokeStyle='rgba(255,90,90,0.85)';ctx.lineWidth=4;\n"
        "for(var i=1;i<o.d.length;i++){if(o.sat[i]>=5)continue;\n"
        "ctx.beginPath();\n"
        "ctx.moveTo(mL+o.d[i-1]/maxd*W,8+H-(o.v[i-1]-minv)/(maxv-minv)*H);\n"
        "ctx.lineTo(mL+o.d[i]/maxd*W,8+H-(o.v[i]-minv)/(maxv-minv)*H);ctx.stroke();}}\n"
        "plotBad(A);plotBad(B);\n"
        /* cursor sincronizado grafico <-> mapa */
        "if(cursD!==null){var cx=mL+cursD/maxd*W;\n"
        "ctx.strokeStyle='#f5f8f5';ctx.lineWidth=1;ctx.beginPath();\n"
        "ctx.moveTo(cx,8);ctx.lineTo(cx,8+H);ctx.stroke();}\n"
        "drawMap();drawDelta(A,B);\n"
        /* painel de stats - frases diretas, sem jargao A/B */
        "function fmt(o){return '<b>'+(o.time/1000).toFixed(3)+'s</b> &middot; vel max '+\n"
        "Math.round(o.vmax)+' km/h &middot; vel media '+(o.vcnt?Math.round(o.vsum/o.vcnt):0)+' km/h';}\n"
        "var dif=(B.time-A.time)/1000,an=selA.value,bn=selB.value;\n"
        "var verd=dif>0?('a volta '+bn+' foi <b style=color:#ff5a5a>'+dif.toFixed(3)+'s mais lenta</b> que a volta '+an)\n"
        ":dif<0?('a volta '+bn+' foi <b style=color:#3ee07a>'+(-dif).toFixed(3)+'s mais rapida</b> que a volta '+an)\n"
        ":'as duas voltas empataram';\n"
        "stats.innerHTML='<span style=color:#3ee07a><b>Volta '+an+' (verde)</b></span>: '+fmt(A)+'<br>'+\n"
        "'<span style=color:#3ec6e0><b>Volta '+bn+' (ciano)</b></span>: '+fmt(B)+'<br>'+\n"
        "'&rarr; '+verd;\n"
        "insights(A,B);\n"
        "}\n"
        /* ---- motor de insights (deterministico, sem IA): setores, curvas
         * (minimos locais da curva suavizada), ponto de frenagem e retas.
         * Convencao: frases descrevem a volta B em relacao a A. ---- */
        "function corners(o){var v=o.v,sm=[],res=[];\n"
        "for(var i=0;i<v.length;i++){var a=0,n=0;\n"
        "for(var j=Math.max(0,i-2);j<=Math.min(v.length-1,i+2);j++){a+=v[j];n++;}sm.push(a/n);}\n"
        "for(var i=5;i<sm.length-5;i++){\n"
        "if(sm[i]<=sm[i-1]&&sm[i]<=sm[i+1]){\n"
        "var pm=0;for(var j=Math.max(0,i-25);j<i;j++)if(sm[j]>pm)pm=sm[j];\n"
        "if(pm-sm[i]>8){res.push({d:o.d[i],v:sm[i],i:i,pm:pm});i+=10;}\n"
        "}}return res;}\n"
        "function brakeD(o,c){for(var i=c.i;i>0;i--){if(o.v[i]>=c.pm-3)return o.d[i];}return c.d;}\n"
        "function nearest(list,d){var best=null;list.forEach(function(c){\n"
        "if(Math.abs(c.d-d)<40&&(!best||Math.abs(c.d-d)<Math.abs(best.d-d)))best=c;});return best;}\n"
        "function segTimes(o){if(!o.s1t||!o.s2t||o.s2t<=o.s1t||o.time<=o.s2t)return null;\n"
        "return [o.s1t/1000,(o.s2t-o.s1t)/1000,(o.time-o.s2t)/1000];}\n"
        "function insights(A,B){\n"
        "var out=[],an=selA.value,bn=selB.value;\n"
        /* setores: maior perda/ganho primeiro */
        "var ga=segTimes(A),gb=segTimes(B);\n"
        "if(ga&&gb){var sd=[];for(var i=0;i<3;i++)sd.push({n:'S'+(i+1),d:gb[i]-ga[i]});\n"
        "sd.sort(function(x,y){return Math.abs(y.d)-Math.abs(x.d);});\n"
        "sd.forEach(function(s){if(Math.abs(s.d)>=0.05)\n"
        "out.push('<b>'+s.n+'</b>: a volta '+bn+' '+(s.d>0?'perde':'ganha')+' <b>'+\n"
        "Math.abs(s.d).toFixed(3)+'s</b> nesse setor vs a volta '+an);});}\n"
        /* curvas: velocidade minima + ponto de frenagem */
        "var ca=corners(A);\n"
        "ca.forEach(function(c){var m=nearest(corners(B),c.d);if(!m)return;\n"
        "var dv=m.v-c.v;\n"
        "if(Math.abs(dv)>=3)out.push('Curva aos ~'+Math.round(c.d)+'m: a volta '+bn+' contorna <b>'+\n"
        "Math.abs(dv).toFixed(0)+' km/h '+(dv<0?'mais devagar':'mais rapido')+'</b> que a volta '+an+' ('+\n"
        "Math.round(m.v)+' vs '+Math.round(c.v)+' km/h)');\n"
        "var db=brakeD(B,m)-brakeD(A,c);\n"
        "if(Math.abs(db)>=8)out.push('Curva aos ~'+Math.round(c.d)+'m: a volta '+bn+' freia <b>'+\n"
        "Math.abs(db).toFixed(0)+'m '+(db<0?'mais cedo':'mais tarde')+'</b> que a volta '+an);});\n"
        /* retas: pico de velocidade */
        "var dvm=B.vmax-A.vmax;\n"
        "if(Math.abs(dvm)>=3)out.push('Reta: a volta '+bn+' chega a <b>'+Math.abs(dvm).toFixed(0)+' km/h '+\n"
        "(dvm<0?'a menos':'a mais')+'</b> no pico que a volta '+an+' ('+Math.round(B.vmax)+' vs '+Math.round(A.vmax)+\n"
        "' km/h) - '+(dvm<0?'saida da curva anterior pior':'melhor tracao na saida'));\n"
        /* qualidade de dados: % da volta com sats<5 */
        "function pctBad(o){if(!o.sat.length)return 0;var b=0;\n"
        "o.sat.forEach(function(s){if(s<5)b++;});return 100*b/o.sat.length;}\n"
        "[[A,selA.value],[B,selB.value]].forEach(function(p){var pb=pctBad(p[0]);\n"
        "if(pb>=5)out.push('<span style=color:#ff5a5a>Atencao:</span> '+pb.toFixed(0)+\n"
        "'% da volta '+p[1]+' com sinal GPS fraco (sats&lt;5, trechos em vermelho) - dados dali sao pouco confiaveis');});\n"
        "if(!out.length)out.push('Voltas praticamente identicas nos criterios analisados (setores, curvas, frenagens, retas).');\n"
        "ins.innerHTML=out.slice(0,10).map(function(t){return '<li>'+t+'</li>';}).join('');\n"
        "}\n"
        /* ---- mapa da pista sincronizado com o grafico ---- */
        "var cursD=null;\n"
        "function idxAt(o,d){var i=0;while(i<o.d.length-1&&o.d[i]<d)i++;return i;}\n"
        "function drawMap(){\n"
        "var A=laps[selA.value],B=laps[selB.value];if(!A||!B)return;\n"
        "var m=mv.getContext('2d');m.clearRect(0,0,mv.width,mv.height);\n"
        "var xs=A.x.concat(B.x),ys=A.y.concat(B.y);\n"
        "if(!xs.length)return;\n"
        "var x0=Math.min.apply(null,xs),x1=Math.max.apply(null,xs);\n"
        "var y0=Math.min.apply(null,ys),y1=Math.max.apply(null,ys);\n"
        "var sx=(x1-x0)||1,sy=(y1-y0)||1,mg=24;\n"
        "var sc=Math.min((mv.width-2*mg)/sx,(mv.height-2*mg)/sy);\n"
        "var offx=mg+((mv.width-2*mg)-sx*sc)/2,offy=mg+((mv.height-2*mg)-sy*sc)/2;\n"
        "function px(x){return offx+(x-x0)*sc;}\n"
        "function py(y){return offy+((y1-y0)-(y-y0))*sc;}\n"
        "function trace(o,col,w){m.strokeStyle=col;m.lineWidth=w;m.beginPath();\n"
        "for(var i=0;i<o.x.length;i++){var X=px(o.x[i]),Y=py(o.y[i]);\n"
        "if(i)m.lineTo(X,Y);else m.moveTo(X,Y);}m.stroke();}\n"
        "trace(B,'#3ec6e0',2);trace(A,'#3ee07a',2);\n"
        "if(cursD!==null){\n"
        "var ia=idxAt(A,cursD),ib=idxAt(B,cursD);\n"
        "m.fillStyle='#3ee07a';m.beginPath();m.arc(px(A.x[ia]),py(A.y[ia]),7,0,7);m.fill();\n"
        "m.fillStyle='#3ec6e0';m.beginPath();m.arc(px(B.x[ib]),py(B.y[ib]),7,0,7);m.fill();\n"
        "m.strokeStyle='#f5f8f5';m.lineWidth=1;\n"
        "m.beginPath();m.arc(px(A.x[ia]),py(A.y[ia]),9,0,7);m.stroke();\n"
        "}\n"
        "}\n"
        "function cursMove(ev){\n"
        "var A=laps[selA.value],B=laps[selB.value];if(!A||!B)return;\n"
        "var r=cv.getBoundingClientRect();\n"
        "var clX=(ev.touches?ev.touches[0].clientX:ev.clientX);\n"
        "var xpx=(clX-r.left)*(cv.width/r.width);\n"
        "var maxd=Math.max(A.last,B.last)||1;\n"
        "var d=(xpx-56)/(cv.width-56-12)*maxd;\n"
        "cursD=Math.max(0,Math.min(maxd,d));\n"
        "draw();\n"
        "if(ev.touches)ev.preventDefault();\n"
        "}\n"
        "cv.addEventListener('mousemove',cursMove);\n"
        "cv.addEventListener('touchmove',cursMove,{passive:false});\n"
        "cv.addEventListener('mouseleave',function(){cursD=null;draw();});\n"
        "function drawDelta(A,B){\n"
        "var c=dc.getContext('2d');c.clearRect(0,0,dc.width,dc.height);\n"
        "var mL=56,W=dc.width-mL-12,H=dc.height-40,maxd=Math.max(A.last,B.last)||1;\n"
        "var pts=[],j=0,mx=0.05;\n"
        "for(var i=0;i<A.d.length;i++){var d=A.d[i];\n"
        "while(j<B.d.length-1&&B.d[j]<d)j++;\n"
        "var tb;\n"
        "if(j==0||B.d[j]<=B.d[j-1])tb=B.t[j];\n"
        "else{var f=(d-B.d[j-1])/(B.d[j]-B.d[j-1]);f=f<0?0:(f>1?1:f);tb=B.t[j-1]+(B.t[j]-B.t[j-1])*f;}\n"
        "var dl=(tb-A.t[i])/1000;pts.push([d,dl]);if(Math.abs(dl)>mx)mx=Math.abs(dl);}\n"
        "mx=Math.ceil(mx*10)/10;\n"
        "var zy=8+H/2;\n"
        "function Y(v){return zy-v/mx*(H/2);}\n"
        "c.font='15px monospace';\n"
        "[-1,-0.5,0,0.5,1].forEach(function(g){var y=Y(g*mx);\n"
        "c.strokeStyle=g==0?'#5a5f58':'#262626';c.beginPath();c.moveTo(mL,y);c.lineTo(mL+W,y);c.stroke();\n"
        "c.fillStyle='#7a847a';c.fillText((g*mx>0?'+':'')+(g*mx).toFixed(1),6,y+5);});\n"
        "c.fillStyle='#7a847a';\n"
        "c.fillText('delta (s): B - A',mL+8,22);\n"
        "c.fillText('acima = B atras \\u00b7 abaixo = B na frente',mL+W-330,22);\n"
        "c.fillText('distancia na volta \\u2192',mL+W-190,dc.height-8);\n"
        "c.lineWidth=2.5;\n"
        "for(var i=1;i<pts.length;i++){\n"
        "c.strokeStyle=pts[i][1]>0?'#ff5a5a':'#3ee07a';\n"
        "c.beginPath();c.moveTo(mL+pts[i-1][0]/maxd*W,Y(pts[i-1][1]));\n"
        "c.lineTo(mL+pts[i][0]/maxd*W,Y(pts[i][1]));c.stroke();}\n"
        "if(cursD!==null){var cx=mL+cursD/maxd*W;\n"
        "c.strokeStyle='#f5f8f5';c.lineWidth=1;c.beginPath();\n"
        "c.moveTo(cx,8);c.lineTo(cx,8+H);c.stroke();}\n"
        "}\n"
        "dc.addEventListener('mousemove',cursMove);\n"
        "dc.addEventListener('touchmove',cursMove,{passive:false});\n"
        "dc.addEventListener('mouseleave',function(){cursD=null;draw();});\n"
        "function mmss(ms){var s=ms/1000,m=Math.floor(s/60);var r=(s-m*60).toFixed(3);if(r.length<6)r='0'+r;return m+':'+r;}\n"
        "function shareCard(){\n"
        "var nums=Object.keys(laps).map(Number).sort(function(a,b){return a-b;});\n"
        "if(!nums.length)return;\n"
        "var ts=nums.map(function(n){return laps[n].time;});\n"
        "var bt=Math.min.apply(null,ts),bn=nums[ts.indexOf(bt)];\n"
        "var clean=ts.filter(function(t){return t<=bt*1.10;});\n"
        "var avg=clean.reduce(function(a,b){return a+b;},0)/clean.length;\n"
        "var sd=Math.sqrt(clean.reduce(function(a,b){return a+(b-avg)*(b-avg);},0)/clean.length);\n"
        "var vmax=0;nums.forEach(function(n){if(laps[n].vmax>vmax)vmax=laps[n].vmax;});\n"
        "var f=new URLSearchParams(location.search).get('file')||'';\n"
        "var m=f.match(/(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})(?:_(.+))?\\.csv$/);\n"
        "var dt=m?m[3]+'/'+m[2]+'/'+m[1]+' '+m[4]+':'+m[5]:'';\n"
        "var trk=m&&m[7]?m[7].replace(/_/g,' '):'';\n"
        "var cn=document.createElement('canvas');cn.width=1080;cn.height=1350;\n"
        "var g=cn.getContext('2d');\n"
        "g.fillStyle='#0d0f0d';g.fillRect(0,0,1080,1350);\n"
        "g.fillStyle='#3ee07a';g.fillRect(0,0,1080,10);\n"
        "g.fillStyle='#3ee07a';g.font='bold 44px sans-serif';g.fillText('KARTBOX',60,100);\n"
        "g.fillStyle='#f5f8f5';g.font='bold 52px sans-serif';g.fillText(trk||'Sessao',60,180,960);\n"
        "g.fillStyle='#7a847a';g.font='32px sans-serif';g.fillText(dt,60,228);\n"
        "g.fillStyle='#7a847a';g.font='30px sans-serif';g.fillText('MELHOR VOLTA (volta '+bn+')',60,320);\n"
        "g.fillStyle='#3ee07a';g.font='bold 150px sans-serif';g.fillText(mmss(bt),52,460);\n"
        "var st=[['VOLTAS',''+nums.length],['MEDIA',mmss(avg)],['CONSISTENCIA','\\u00b1'+(sd/1000).toFixed(2)+'s'],['VEL. MAX',Math.round(vmax)+' km/h']];\n"
        "for(var i=0;i<4;i++){var x=60+(i%2)*500,y=540+Math.floor(i/2)*150;\n"
        "g.fillStyle='#7a847a';g.font='26px sans-serif';g.fillText(st[i][0],x,y);\n"
        "g.fillStyle='#f5f8f5';g.font='bold 54px sans-serif';g.fillText(st[i][1],x,y+62);}\n"
        "var B=laps[bn];\n"
        "if(B&&B.x.length){\n"
        "var x0=Math.min.apply(null,B.x),x1=Math.max.apply(null,B.x),y0=Math.min.apply(null,B.y),y1=Math.max.apply(null,B.y);\n"
        "var sx=(x1-x0)||1,sy=(y1-y0)||1,ax=60,ay=860,aw=960,ah=420;\n"
        "var sc=Math.min(aw/sx,ah/sy);\n"
        "var ox=ax+(aw-sx*sc)/2,oy=ay+(ah-sy*sc)/2;\n"
        "g.strokeStyle='#3ee07a';g.lineWidth=6;g.lineJoin='round';g.beginPath();\n"
        "for(var i=0;i<B.x.length;i++){var X=ox+(B.x[i]-x0)*sc,Y=oy+(y1-B.y[i])*sc;\n"
        "if(i)g.lineTo(X,Y);else g.moveTo(X,Y);}\n"
        "g.stroke();}\n"
        "g.fillStyle='#7a847a';g.font='24px sans-serif';g.fillText('tracado da melhor volta',60,1320);\n"
        "cn.toBlob(function(b){\n"
        "var file=new File([b],'kartbox_resumo.png',{type:'image/png'});\n"
        "if(navigator.canShare&&navigator.canShare({files:[file]})){\n"
        "navigator.share({files:[file],title:'KartBox'}).catch(function(){});}\n"
        "else{var a=document.createElement('a');a.href=URL.createObjectURL(b);\n"
        "a.download='kartbox_resumo.png';document.body.appendChild(a);a.click();a.remove();}\n"
        "},'image/png');\n"
        "}\n"
        /* ---- insights da SESSAO inteira (vs melhor volta) - roda uma vez
         * no load. Voltas >10% acima do best sao tratadas como fora de
         * ritmo (transito/rodada) e excluidas das medias. ---- */
        "function avg(a){return a.reduce(function(x,y){return x+y;},0)/a.length;}\n"
        "function sessionInsights(nums,bestN){\n"
        "var out=[];var bt=laps[bestN].time;\n"
        "var clean=nums.filter(function(n){return laps[n].time>0&&laps[n].time<=bt*1.10;});\n"
        "var outl=nums.filter(function(n){return laps[n].time>bt*1.10;});\n"
        "out.push('Best: <b>volta '+bestN+'</b> de '+nums.length+' ('+(bt/1000).toFixed(3)+'s)');\n"
        "if(outl.length)out.push('Fora de ritmo (transito/erro? ignoradas das medias): volta(s) '+outl.join(', '));\n"
        "if(clean.length>=3){\n"
        "var t=clean.map(function(n){return laps[n].time/1000;});\n"
        "var m=avg(t),sd=Math.sqrt(avg(t.map(function(x){return (x-m)*(x-m);})));\n"
        "out.push('Consistencia (voltas limpas): <b>&plusmn;'+sd.toFixed(3)+'s</b> - '+\n"
        "(sd<0.3?'excelente':sd<0.7?'boa':'irregular: ritmo constante rende mais que forcar volta unica'));\n"
        /* fadiga/evolucao: metade inicial vs final das voltas limpas */
        "var half=Math.floor(t.length/2),a1=avg(t.slice(0,half)),a2=avg(t.slice(half));\n"
        "if(a2-a1>=0.15)out.push('Queda de rendimento a partir da <b>volta ~'+clean[half]+'</b>: '+\n"
        "'metade final em media <b>+'+(a2-a1).toFixed(2)+'s/volta</b> - fadiga, pneus ou concentracao');\n"
        "else if(a1-a2>=0.15)out.push('Evolucao durante a sessao: metade final <b>'+\n"
        "(a1-a2).toFixed(2)+'s/volta mais rapida</b> - seguiu aprendendo a pista');\n"
        "}\n"
        /* perda media por setor vs best + volta ideal */
        "var segsB=segTimes(laps[bestN]);\n"
        "if(segsB){var loss=[0,0,0],cnt=0,mins=[1e9,1e9,1e9];\n"
        "clean.forEach(function(n){var g=segTimes(laps[n]);if(!g)return;cnt++;\n"
        "for(var i=0;i<3;i++){loss[i]+=g[i]-segsB[i];if(g[i]<mins[i])mins[i]=g[i];}});\n"
        "if(cnt>1){for(var i=0;i<3;i++)loss[i]/=cnt;\n"
        "var wi=0;for(var i=1;i<3;i++)if(loss[i]>loss[wi])wi=i;\n"
        "if(loss[wi]>0.05)out.push('Maior fonte de perda: <b>S'+(wi+1)+'</b>, em media <b>+'+\n"
        "loss[wi].toFixed(3)+'s/volta</b> vs sua melhor volta - foco numero 1');\n"
        "var ideal=mins[0]+mins[1]+mins[2];\n"
        "if(ideal<bt/1000-0.05)out.push('Volta ideal (juntando seus melhores setores): <b>'+\n"
        "ideal.toFixed(3)+'s</b>, '+((bt/1000)-ideal).toFixed(3)+'s abaixo do best - '+\n"
        "'o ritmo existe, falta juntar tudo na mesma volta');\n"
        "}}\n"
        /* curva-problema recorrente: deficit medio de velocidade minima
         * vs a melhor volta, curva a curva */
        "if(clean.length>=3){\n"
        "var cb=corners(laps[bestN]);\n"
        "if(cb.length){var cbl={};clean.forEach(function(n){if(n!=bestN)cbl[n]=corners(laps[n]);});\n"
        "var wc=null;\n"
        "cb.forEach(function(c){var defs=[];\n"
        "clean.forEach(function(n){if(n==bestN)return;var m=nearest(cbl[n],c.d);if(m)defs.push(c.v-m.v);});\n"
        "if(defs.length>=2){var dm=avg(defs);if(dm>=3&&(!wc||dm>wc.dm))wc={d:c.d,dm:dm};}});\n"
        "if(wc)out.push('Curva recorrente: na <b>~'+Math.round(wc.d)+'m</b> voce passa em media <b>'+\n"
        "wc.dm.toFixed(0)+' km/h abaixo</b> da sua melhor passagem - maior oportunidade da sessao');\n"
        "}}\n"
        "gins.innerHTML=out.map(function(x){return '<li>'+x+'</li>';}).join('');\n"
        "}\n"
        "selA.onchange=selB.onchange=draw;load();\n"
        "</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * APIs leves pra pagina Evolucao (agregado por sessao SEM baixar o CSV
 * inteiro no navegador - o resumo por volta ja existe no firmware via
 * sd_read_session_laps, entao servimos so ele).
 *   GET /api/sessions        -> nomes, um por linha (recentes primeiro)
 *   GET /api/laps?file=X.csv -> "lap,time_ms,s1_ms,s2_ms,vmax,vmed"
 * --------------------------------------------------------------------- */
static void add_cors_headers(httpd_req_t *req); /* definida junto da API de pistas, mais abaixo */
static esp_err_t api_sessions_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");
    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int n = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);
    for (int i = 0; i < n; i++) {
        httpd_resp_sendstr_chunk(req, sessions[i].filename);
        httpd_resp_sendstr_chunk(req, "\n");
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t api_laps_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);

    char query[160], fname[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "file", fname, sizeof(fname));
    }
    if (!web_filename_ok(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file invalido");
        return ESP_FAIL;
    }

    static sd_lap_summary_t lapbuf[SD_MAX_LAPS_LISTED];
    int n = sd_read_session_laps(fname, lapbuf, SD_MAX_LAPS_LISTED);

    httpd_resp_set_type(req, "text/plain");
    char line[96];
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "%lu,%lu,%lu,%lu,%.1f,%.1f\n",
                 (unsigned long)lapbuf[i].lap_number,
                 (unsigned long)lapbuf[i].lap_time_ms,
                 (unsigned long)lapbuf[i].sector_ms[0],
                 (unsigned long)lapbuf[i].sector_ms[1],
                 (double)lapbuf[i].max_speed_kmh,
                 (double)lapbuf[i].avg_speed_kmh);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * GET /relatorio?file=X - relatorio completo da sessao: analisa TODAS as
 * voltas (nao so A x B). Alinha cada volta a melhor pela posicao na pista,
 * detecta curvas (minimos de velocidade), incidentes (paradas/rodadas),
 * perda por trecho acima do ritmo geral, setores e consistencia. Cada
 * afirmacao traz o numero medido e o tamanho da amostra.
 * 100% no navegador: baixa o CSV cru via /download, como /analise.
 * --------------------------------------------------------------------- */
static esp_err_t report_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>KartBox - relatorio</title><style>\n"
        "body{margin:0 auto;padding:16px;max-width:1000px;background:#0d0f0d;color:#f5f8f5;font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif}\n"
        "h1{font-size:18px;color:#3ee07a;border-bottom:2px solid #262626;padding-bottom:8px}\n"
        "h3{color:#7a847a;font-size:13px;text-transform:uppercase;letter-spacing:1px;margin:22px 0 6px}\n"
        "a{color:#3ec6e0;text-decoration:none}\n"
        "canvas{width:100%;background:#141714;border:1px solid #262626;border-radius:8px}\n"
        "table{width:100%;border-collapse:collapse;font-size:13px}\n"
        "th,td{padding:6px 7px;border-bottom:1px solid #262626;text-align:left}\n"
        "th{color:#7a847a;font-size:11px;text-transform:uppercase}\n"
        "tr.best td{color:#3ee07a}\n"
        "ul{margin:4px 0;padding-left:20px;font-size:15px;line-height:1.65}\n"
        "li{margin-bottom:9px}\n"
        "#err{color:#ff5a5a}.lg{font-size:13px;color:#7a847a;margin:8px 0}\n"
        "button{background:#141714;color:#f5f8f5;border:1px solid #262626;border-radius:8px;padding:8px 14px;font-size:14px;cursor:pointer}\n"
        "@media print{body{background:#fff;color:#000;max-width:100%}h1{color:#000;border-color:#999}h3{color:#333}.noprint{display:none!important}canvas{border:1px solid #999;-webkit-print-color-adjust:exact;print-color-adjust:exact}th,td{border-color:#ccc}li{break-inside:avoid}}\n"
        "</style></head><body>\n"
        "<p class=\"noprint\"><a href=\"/\">&larr; sessoes</a> &middot; <a id=\"lnk\" href=\"#\">comparar duas voltas</a></p>\n"
        "<h1 id=\"hd\">Relatorio completo</h1>\n"
        "<div class=\"noprint\"><button onclick=\"window.print()\">Imprimir relatorio</button></div>\n"
        "<div id=\"stats\" class=\"lg\" style=\"font-size:15px;color:#f5f8f5\"></div>\n"
        "<h3>Prioridades: onde esta o tempo</h3>\n"
        "<ul id=\"pri\"></ul>\n"
        "<h3>Incidentes e voltas atipicas</h3>\n"
        "<ul id=\"inc\"></ul>\n"
        "<h3>Consistencia e tendencia</h3>\n"
        "<ul id=\"vr\"></ul>\n"
        "<h3>Tempo por volta</h3>\n"
        "<canvas id=\"lt\" width=\"1200\" height=\"300\"></canvas>\n"
        "<h3>Mapa: perda media por trecho</h3>\n"
        "<canvas id=\"mp\" width=\"1200\" height=\"560\"></canvas>\n"
        "<p class=\"lg\">cor = tempo perdido em media em cada trecho (verde escuro = nada relevante, amarelo = medio, vermelho = muito, cinza = poucas voltas para comparar) &middot; numeros = curvas; contorno amarelo = as 3 prioridades</p>\n"
        "<h3>Perda por curva em cada volta</h3>\n"
        "<canvas id=\"hm\" width=\"1200\" height=\"300\"></canvas>\n"
        "<p id=\"hi\" class=\"lg noprint\">toque/passe o mouse numa celula para ver o detalhe</p>\n"
        "<p class=\"lg\">cada coluna e um trecho da pista (C = curva, R = reta), cada linha uma volta. Numero = segundos perdidos contra a melhor passagem de qualquer volta naquele trecho. &quot;x&quot; = trecho excluido (incidente, volta lenta ou box).</p>\n"
        "<h3>Curvas: medicoes</h3>\n"
        "<table id=\"ct\"></table>\n"
        "<p class=\"lg\">Frenagem = onde comecou a perder velocidade antes da curva, nas voltas com perda, contra a melhor passagem. Explica = parte da variacao do tempo de volta que vem desse trecho (so com 5+ voltas limpas).</p>\n"
        "<h3>Setores</h3>\n"
        "<table id=\"sc\"></table>\n"
        "<h3>Tabela de voltas</h3>\n"
        "<table id=\"tb\"></table>\n"
        "<h3>Volta a volta: onde cada uma perdeu</h3>\n"
        "<ul id=\"pl\"></ul>\n"
        "<h3>Qualidade dos dados e metodo</h3>\n"
        "<ul id=\"dq\"></ul>\n"
        "<p id=\"err\"></p>\n"
        "<script>\n"
        "");
    httpd_resp_sendstr_chunk(req,
        "var laps={},R={};\n"
        "function $(i){return document.getElementById(i);}\n"
        "function fmt(ms){var s=ms/1000,m=Math.floor(s/60),r=(s-m*60).toFixed(3);if(r.length<6)r='0'+r;return m+':'+r;}\n"
        "function sg(x){return (x>=0?'+':'-')+Math.abs(x).toFixed(2)+'s';}\n"
        "function sk(x){var r=Math.round(x);return (r>0?'+':'')+(r===0?0:r);}\n"
        "function avg(a){return a.length?a.reduce(function(x,y){return x+y;},0)/a.length:0;}\n"
        "function sdv(a){var m=avg(a);return Math.sqrt(avg(a.map(function(x){return (x-m)*(x-m);})));}\n"
        "function bg(c){var g=c.getContext('2d');g.fillStyle='#141714';g.fillRect(0,0,c.width,c.height);return g;}\n"
        "function load(){\n"
        "var f=new URLSearchParams(location.search).get('file');\n"
        "if(!f){$('err').textContent='sem arquivo na URL';return;}\n"
        "$('hd').textContent='Relatorio completo - '+f;\n"
        "$('lnk').href='/analise?file='+encodeURIComponent(f);\n"
        "fetch('/download?file='+encodeURIComponent(f)).then(function(r){return r.text();}).then(function(csv){\n"
        "var L=csv.trim().split(/\\r?\\n/),prev=null,ox=null,oy=null,okx=1,pt=0;\n"
        "for(var i=1;i<L.length;i++){var c=L[i].split(',');if(c.length<8)continue;\n"
        "var lap=+c[6];if(lap<1)continue;\n"
        "var lat=+c[1],lon=+c[2],v=+c[3],t=+c[7],sat=+c[5]||0;\n"
        "if(lat==0&&lon==0)continue;\n"
        "if(ox===null){ox=lon;oy=lat;okx=111320*Math.cos(lat*Math.PI/180);}\n"
        "var o=laps[lap]||(laps[lap]={d:[],v:[],t:[],x:[],y:[],sat:[],last:0,time:0,s1t:0,s2t:0,gaps:0});\n"
        "if(prev&&prev.lap==lap){var kx=111320*Math.cos(lat*Math.PI/180);\n"
        "var dx=(lon-prev.lon)*kx,dy=(lat-prev.lat)*111320;o.last+=Math.sqrt(dx*dx+dy*dy);\n"
        "if(t-o.time>400)o.gaps++;}\n"
        "o.d.push(o.last);o.v.push(v);o.t.push(t);o.time=t;\n"
        "o.x.push((lon-ox)*okx);o.y.push((lat-oy)*111320);o.sat.push(sat);\n"
        "if(c.length>=12){if(+c[10]>0)o.s1t=+c[10];if(+c[11]>0)o.s2t=+c[11];}\n"
        "prev={lat:lat,lon:lon,lap:lap};}\n"
        "var ks=Object.keys(laps).map(Number);\n"
        "if(ks.length)delete laps[Math.max.apply(null,ks)];\n"
        "build();\n"
        "}).catch(function(e){$('err').textContent='falha ao gerar o relatorio ('+e+')';});\n"
        "}\n"
        "function lb(a,x){var lo=0,hi=a.length;while(lo<hi){var m=(lo+hi)>>1;if(a[m]>=x)hi=m;else lo=m+1;}return lo;}\n"
        "function tAt(o,g){var rd=o.rd,n=rd.length;\n"
        "if(g<=rd[0])return o.t[0];if(g>=rd[n-1])return o.t[n-1];\n"
        "var i=lb(rd,g),d0=rd[i-1],d1=rd[i],q=d1>d0?(g-d0)/(d1-d0):0;\n"
        "return o.t[i-1]+(o.t[i]-o.t[i-1])*q;}\n"
        "function align(o,Rf){\n"
        "var n=o.x.length,m=Rf.x.length,rd=new Array(n),j=0,W=90,ds=0;\n"
        "for(var i=0;i<n;i++){\n"
        "var lo=Math.max(0,j-3),hi=Math.min(m-2,j+W),bd=1e18,bj=lo,bq=0;\n"
        "for(var k=lo;k<=hi;k++){\n"
        "var ax=Rf.x[k],ay=Rf.y[k],bx=Rf.x[k+1]-ax,by=Rf.y[k+1]-ay,l2=bx*bx+by*by;\n"
        "var q=l2>0?((o.x[i]-ax)*bx+(o.y[i]-ay)*by)/l2:0;q=q<0?0:(q>1?1:q);\n"
        "var px=ax+q*bx-o.x[i],py=ay+q*by-o.y[i],dd=px*px+py*py;\n"
        "if(dd<bd){bd=dd;bj=k;bq=q;}}\n"
        "j=bj;ds+=Math.sqrt(bd);\n"
        "var d=Rf.d[bj]+bq*(Rf.d[bj+1]-Rf.d[bj]);\n"
        "rd[i]=i?Math.max(rd[i-1],d):d;}\n"
        "o.rd=rd;o.dev=ds/n;}\n"
        "function zigzag(s,prom){\n"
        "var ext=[],mx=s[0],mxi=0,mn=s[0],mni=0,dir=0;\n"
        "for(var i=1;i<s.length;i++){var v=s[i];\n"
        "if(dir==0){if(v>mx){mx=v;mxi=i;}if(v<mn){mn=v;mni=i;}\n"
        "if(mx-v>=prom){ext.push({i:mxi,v:mx,k:1});dir=-1;mn=v;mni=i;}\n"
        "else if(v-mn>=prom){ext.push({i:mni,v:mn,k:-1});dir=1;mx=v;mxi=i;}}\n"
        "else if(dir==1){if(v>mx){mx=v;mxi=i;}else if(mx-v>=prom){ext.push({i:mxi,v:mx,k:1});dir=-1;mn=v;mni=i;}}\n"
        "else{if(v<mn){mn=v;mni=i;}else if(v-mn>=prom){ext.push({i:mni,v:mn,k:-1});dir=1;mx=v;mxi=i;}}}\n"
        "return ext;}\n"
        "function makeTiles(Rf){\n"
        "var step=2,len=Rf.last,G=Math.floor(len/step)+1,s=[],j=0;\n"
        "for(var g=0;g<G;g++){var d=g*step;while(j<Rf.d.length-2&&Rf.d[j+1]<d)j++;\n"
        "var d0=Rf.d[j],d1=Rf.d[j+1],q=d1>d0?(d-d0)/(d1-d0):0;q=q<0?0:(q>1?1:q);\n"
        "s.push(Rf.v[j]+(Rf.v[j+1]-Rf.v[j])*q);}\n"
        "var sm=s.map(function(_,i){var a=0,c=0;for(var k=Math.max(0,i-5);k<=Math.min(G-1,i+5);k++){a+=s[k];c++;}return a/c;});\n"
        "var vmax=Math.max.apply(null,sm),prom=Math.max(4,0.10*vmax);\n"
        "var ext=zigzag(sm,prom),cuts=[0];\n"
        "ext.forEach(function(e){if(e.k==1){var d=e.i*step;if(d-cuts[cuts.length-1]>=12&&len-d>=12)cuts.push(d);}});\n"
        "cuts.push(len);\n"
        "var tiles=[],cn=0,rn=0;\n"
        "for(var i=0;i<cuts.length-1;i++){var a=cuts[i],b=cuts[i+1],ap=null,av=1e9;\n"
        "ext.forEach(function(e){var d=e.i*step;if(e.k==-1&&d>=a&&d<=b&&e.v<av){av=e.v;ap=d;}});\n"
        "var t={a:a,b:b,apex:ap,k:i};\n"
        "if(ap!==null){t.type='c';t.num=++cn;t.name='Curva '+t.num;}else{t.type='r';t.num=++rn;t.name='Reta '+t.num;}\n"
        "tiles.push(t);}\n"
        "return tiles;}\n"
        "function tmet(o,tl){\n"
        "var rd=o.rd,v=o.v,n=rd.length,ta=tAt(o,tl.a),tb=tAt(o,tl.b);\n"
        "var m={t:(tb-ta)/1000};\n"
        "var i0=lb(rd,tl.a),i1=Math.min(lb(rd,tl.b),n-1);if(i0>i1)i0=i1;\n"
        "var vmn=1e9,dmn=tl.a,vx=0,sum=0,c=0;\n"
        "for(var i=i0;i<=i1;i++){if(v[i]<vmn){vmn=v[i];dmn=rd[i];}if(v[i]>vx)vx=v[i];sum+=v[i];c++;}\n"
        "m.vmin=vmn;m.vmax=vx;m.vavg=c?sum/c:0;\n"
        "if(tl.apex!==null){\n"
        "m.te=(tAt(o,tl.apex)-ta)/1000;m.tx=m.t-m.te;\n"
        "var vin=0,din=tl.a,vout=0;\n"
        "for(var i=i0;i<=i1;i++){if(rd[i]<=dmn&&v[i]>vin)vin=v[i];if(rd[i]>=dmn&&v[i]>vout)vout=v[i];}\n"
        "for(var i=i0;i<=i1;i++){if(rd[i]<=dmn&&v[i]>=vin-0.5){din=rd[i];break;}}\n"
        "m.vin=vin;m.vout=vout;m.brk=din;m.apx=dmn;\n"
        "var thr=vmn+0.5*(vout-vmn),rec=tl.b-dmn;\n"
        "for(var i=i0;i<=i1;i++){if(rd[i]>=dmn&&v[i]>=thr){rec=rd[i]-dmn;break;}}\n"
        "m.rec=rec;}\n"
        "return m;}\n"
        "function build(){\n"
        "var nums=Object.keys(laps).map(Number).filter(function(n){var o=laps[n];return o.d.length>=20&&o.last>50&&o.time>5000;}).sort(function(a,b){return a-b;});\n"
        "if(nums.length<1){$('err').textContent='nenhuma volta COMPLETA nessa sessao (a ultima volta, encerrada no botao/box, nao conta)';return;}\n"
        "var best=nums.reduce(function(a,b){return laps[b].time<laps[a].time?b:a;});\n"
        "var Rf=laps[best],bt=Rf.time,tiles=makeTiles(Rf),M=200,st=Rf.last/M;\n"
        "var thr=Math.max(0.05,bt/1000*0.0015),incThr=Math.max(0.8,bt/1000*0.02);\n"
        "Rf.rd=Rf.d.slice();\n"
        "nums.forEach(function(n){var o=laps[n];o.n=n;if(n!=best)align(o,Rf);\n"
        "o.cg=[];for(var k=0;k<=M;k++)o.cg.push(tAt(o,k*st));\n"
        "o.tm=tiles.map(function(tl){return tmet(o,tl);});\n"
        "var b=0;o.sat.forEach(function(s){if(s<5)b++;});o.bad=100*b/o.sat.length;\n"
        "var tot=0,cn=0,mx=0;o.v.forEach(function(v){if(v>mx)mx=v;if(v>1){tot+=v;cn++;}});o.vmax=mx;o.vmed=cn?tot/cn:0;});\n"
        "nums.forEach(function(n){var o=laps[n];o.inc=[];\n"
        "var loss=[];for(var k=0;k<M;k++)loss.push((o.cg[k+1]-o.cg[k]-(Rf.cg[k+1]-Rf.cg[k]))/1000);\n"
        "var runs=[],cur=null,gap=0,pend=0;\n"
        "for(var k=0;k<M;k++){var l=loss[k];\n"
        "if(l>0.02){if(!cur)cur={a:k,b:k,s:0};else cur.s+=pend;cur.b=k;cur.s+=l;gap=0;pend=0;}\n"
        "else if(cur){gap++;pend+=l;if(gap>2){runs.push(cur);cur=null;gap=0;pend=0;}}}\n"
        "if(cur)runs.push(cur);\n"
        "runs.forEach(function(r){if(r.s<incThr||n==best)return;\n"
        "var d0=r.a*st,d1=(r.b+1)*st,vl=1e9,vr=1e9,stop=0;\n"
        "for(var i=0;i<o.rd.length;i++){if(o.rd[i]>=d0&&o.rd[i]<=d1){if(o.v[i]<vl)vl=o.v[i];if(o.v[i]<5&&i>0)stop+=(o.t[i]-o.t[i-1])/1000;}}\n"
        "for(var i=0;i<Rf.d.length;i++){if(Rf.d[i]>=d0&&Rf.d[i]<=d1&&Rf.v[i]<vr)vr=Rf.v[i];}\n"
        "var cls='perda concentrada';\n"
        "if(vl<5&&vr>=10)cls='parada';else if(vr>=12&&vl<=0.5*vr)cls='quase parada ou rodada';\n"
        "o.inc.push({a:d0,b:d1,loss:r.s,span:(r.b-r.a+1)/M,vl:vl,vr:vr,stop:stop,cls:cls});});\n"
        "var tl=(o.time-bt)/1000,isum=o.inc.reduce(function(a,x){return a+x.loss;},0);\n"
        "o.slow=o.inc.some(function(x){return x.span>0.7;})||(tl>=0.06*bt/1000&&isum<0.5*tl);\n"
        "o.inout=(n==nums[0]||n==nums[nums.length-1])&&o.time>bt*1.10;\n"
        "o.cls=o.inout?'saida/entrada do box (provavel)':(o.slow?'ritmo geral lento':(o.inc.length?'incidente':'ok'));\n"
        "o.ok=tiles.map(function(t){if(n==best)return true;if(o.slow||o.inout)return false;\n"
        "return !o.inc.some(function(x){return x.a<t.b&&x.b>t.a;});});});\n"
        "var clean=nums.filter(function(n){return laps[n].cls=='ok'&&laps[n].time<=bt*1.10;}),relaxed=false;\n"
        "if(clean.length<3){clean=nums.filter(function(n){return laps[n].cls=='ok'&&laps[n].time<=bt*1.20;});relaxed=true;}\n"
        "tiles.forEach(function(tl,k){\n"
        "var vs=nums.filter(function(n){return laps[n].ok[k];});tl.n=vs.length;tl.loss={};\n"
        "var ref=vs.reduce(function(a,b){return laps[b].tm[k].t<laps[a].tm[k].t?b:a;},vs[0]);\n"
        "tl.ref=ref;tl.best=laps[ref].tm[k].t;\n"
        "vs.forEach(function(n){tl.loss[n]=laps[n].tm[k].t-tl.best;});\n"
        "tl.mean=avg(vs.map(function(n){return tl.loss[n];}));\n"
        "tl.nlose=vs.filter(function(n){return tl.loss[n]>thr;}).length;\n"
        "tl.tmean=avg(vs.map(function(n){return laps[n].tm[k].t;}));\n"
        "if(tl.type=='c'){var rm=laps[ref].tm[k];\n"
        "tl.meanE=avg(vs.map(function(n){return laps[n].tm[k].te-rm.te;}));\n"
        "tl.meanX=avg(vs.map(function(n){return laps[n].tm[k].tx-rm.tx;}));\n"
        "var ls=vs.filter(function(n){return n!=ref&&tl.loss[n]>thr;}),bs=ls.length?ls:vs.filter(function(n){return n!=ref;});\n"
        "var d=function(f){return avg(bs.map(function(n){return f(laps[n].tm[k])-f(rm);}));};\n"
        "tl.dvmin=d(function(m){return m.vmin;});tl.dvin=d(function(m){return m.vin;});tl.dvout=d(function(m){return m.vout;});\n"
        "tl.dbrk=d(function(m){return m.brk;});tl.dapx=d(function(m){return m.apx;});tl.drec=d(function(m){return m.rec;});\n"
        "tl.vminAvg=avg(vs.map(function(n){return laps[n].tm[k].vmin;}));tl.vminBest=rm.vmin;}\n"
        "tl.share=null;});\n"
        "if(clean.length>=5){var T=clean.map(function(n){return laps[n].time;}),mT=avg(T),vT=avg(T.map(function(x){return (x-mT)*(x-mT);}));\n"
        "if(vT>0)tiles.forEach(function(tl,k){var a=clean.map(function(n){return laps[n].tm[k].t*1000;}),ma=avg(a);\n"
        "var cv=avg(a.map(function(x,i){return (x-ma)*(T[i]-mT);}));tl.share=100*cv/vT;});}\n"
        "tiles.forEach(function(t){t.exc=0;t.nex=0;t.excN=0;});\n"
        "nums.forEach(function(n){var o=laps[n],V=tiles.filter(function(t){return o.ok[t.k];}),sb=V.reduce(function(a,t){return a+t.best;},0),tot=0;\n"
        "V.forEach(function(t){tot+=o.tm[t.k].t-t.best;});o.pace=tot;\n"
        "V.forEach(function(t){var e=(o.tm[t.k].t-t.best)-tot*t.best/sb;t.exc+=e;t.excN++;if(e>thr)t.nex++;});});\n"
        "tiles.forEach(function(t){t.exc=t.excN?t.exc/t.excN:0;t.conc=null;\n"
        "var ca=Math.floor(t.a/st),cb=Math.min(M-1,Math.ceil(t.b/st)-1),w=cb-ca+1;\n"
        "if(w<6||t.n<3)return;\n"
        "var vs=nums.filter(function(n){return laps[n].ok[t.k]&&n!=t.ref;}),mc=[],rl=laps[t.ref];\n"
        "if(vs.length<2)return;\n"
        "var rt=rl.time/1000-rl.tm[t.k].t,rate={};\n"
        "vs.forEach(function(n){rate[n]=rt>0?((laps[n].time-rl.time)/1000-(laps[n].tm[t.k].t-rl.tm[t.k].t))/rt:0;});\n"
        "for(var j=ca;j<=cb;j++){var rc=(rl.cg[j+1]-rl.cg[j])/1000;mc.push(avg(vs.map(function(n){return (laps[n].cg[j+1]-laps[n].cg[j])/1000-rc*(1+rate[n]);})));}\n"
        "var tot=mc.reduce(function(a,x){return a+x;},0),ww=Math.max(3,Math.round(w*0.3)),bs=-1e9,bj=0;\n"
        "for(var j=0;j+ww<=w;j++){var s=0;for(var q=0;q<ww;q++)s+=mc[j+q];if(s>bs){bs=s;bj=j;}}\n"
        "if(tot>thr&&bs/tot>=0.6&&ww<w*0.8)t.conc={a:(ca+bj)*st,b:(ca+bj+ww)*st,f:Math.min(1,bs/tot)};});\n"
        "R={nums:nums,best:best,bt:bt,Rf:Rf,tiles:tiles,clean:clean,relaxed:relaxed,thr:thr,incThr:incThr,M:M,st:st};\n"
        "render();}\n"
        "function pos(d){return Math.round(100*d/R.Rf.last);}\n"
        "function span(tl){return pos(tl.a)+'% a '+pos(tl.b)+'%';}\n"
        "function diag(tl){\n"
        "if(tl.type!='c')return 'trecho reto: a perda e de velocidade media menor (nao ha curva definida aqui)';\n"
        "var e=tl.meanE,x=tl.meanX,t=[];\n"
        "if(e>x*1.5&&e>=0.03){\n"
        "if(tl.dbrk<=-5)t.push('freia '+Math.round(-tl.dbrk)+' m mais cedo que na melhor passagem');\n"
        "else if(tl.dbrk>=5)t.push('freia '+Math.round(tl.dbrk)+' m mais tarde que na melhor passagem e paga no meio da curva');\n"
        "else t.push('o ponto de frenagem e praticamente o mesmo');\n"
        "if(tl.dvmin<=-3)t.push('passa '+Math.round(-tl.dvmin)+' km/h mais devagar no apice');}\n"
        "else if(x>e*1.5&&x>=0.03){\n"
        "if(tl.drec>=5)t.push('leva '+Math.round(tl.drec)+' m a mais para recuperar velocidade depois do apice');\n"
        "if(tl.dvout<=-3)t.push('sai '+Math.round(-tl.dvout)+' km/h mais lento');\n"
        "if(tl.dapx<=-5)t.push('o apice vem '+Math.round(-tl.dapx)+' m mais cedo (indicio de apice antecipado)');\n"
        "if(!t.length)t.push('perde tempo na saida sem diferenca clara de velocidade');}\n"
        "else t.push('perda dividida entre entrada e saida');\n"
        "return t.join('; ');}\n"
        "function lapLabel(o){return o.cls=='ok'?'':' ['+o.cls+']';}\n"
        "function render(){\n"
        "var nums=R.nums,best=R.best,bt=R.bt,tiles=R.tiles,clean=R.clean,thr=R.thr;\n"
        "var ct=clean.map(function(n){return laps[n].time;});\n"
        "var av=avg(ct),sd=sdv(ct);\n"
        "var f=new URLSearchParams(location.search).get('file')||'';\n"
        "var m=f.match(/(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})(?:_(.+))?\\.csv$/);\n"
        "var dt=m?m[3]+'/'+m[2]+'/'+m[1]+' '+m[4]+':'+m[5]:'',trk=m&&m[7]?m[7].replace(/_/g,' '):'';\n"
        "var conf=clean.length>=8?'alta':(clean.length>=4?'media':'baixa');\n"
        "$('stats').innerHTML=(trk?'<b>'+trk+'</b> &middot; ':'')+dt+'<br>'+\n"
        "nums.length+' voltas completas &middot; '+clean.length+' limpas (sem incidente, ritmo normal) &middot; melhor <b style=color:#3ee07a>'+fmt(bt)+'</b> (volta '+best+')'+\n"
        "(clean.length>1?' &middot; media das limpas '+fmt(av)+' &middot; variacao &plusmn;'+(sd/1000).toFixed(2)+'s':'')+\n"
        "'<br>Confianca da analise: <b>'+conf+'</b>'+(conf=='baixa'?' - poucas voltas limpas; trate os numeros por curva como indicio, nao como regra.':'');\n"
        "priorities();incidents();variation();table();sectors();perlap();quality();drawLaps();drawMap();drawHeat();ctable();\n"
        "}\n"
        "function priorities(){\n"
        "var tiles=R.tiles,thr=R.thr,out=[];\n"
        "var cand=tiles.filter(function(t){return t.n>=3&&t.exc>=thr;}).sort(function(a,b){return b.exc-a.exc;});\n"
        "R.top=cand.slice(0,3);\n"
        "var isum=0,incLaps=0;R.nums.forEach(function(n){var o=laps[n];if(o.inc.length&&!o.slow&&!o.inout){incLaps++;o.inc.forEach(function(x){isum+=x.loss;});}});\n"
        "var sm=[];\n"
        "if(R.top.length)sm.push('principal alvo: <b>'+R.top[0].name+'</b> (ate '+R.top[0].exc.toFixed(2)+'s/volta acima do seu ritmo geral)');\n"
        "if(incLaps)sm.push('incidentes custaram <b>'+isum.toFixed(1)+'s</b> em '+incLaps+' volta(s)');\n"
        "if(R.clean.length>1)sm.push('a media das voltas limpas esta '+((avg(R.clean.map(function(n){return laps[n].time;}))-R.bt)/1000).toFixed(2)+'s acima da sua melhor');\n"
        "if(sm.length)out.push('<li><b>Resumo:</b> '+sm.join('; ')+'.</li>');\n"
        "R.top.forEach(function(t,i){var gl=t.type=='c'?'Essa curva':'Esse trecho';\n"
        "var s='<b>'+(i+1)+'. '+t.name+'</b> (aos '+span(t)+' da volta): perde em media <b>'+t.mean.toFixed(2)+'s/volta</b> contra a sua melhor passagem (volta '+t.ref+'); <b>'+t.exc.toFixed(2)+'s</b> disso e acima do que o seu ritmo geral da volta explicaria (o resto e ritmo). Acima de '+thr.toFixed(2)+'s em '+t.nlose+' de '+t.n+' voltas.';\n"
        "if(t.type=='c')s+=' Entrada '+sg(t.meanE)+', saida '+sg(t.meanX)+'.';\n"
        "if(t.conc)s+=' Contra a volta '+t.ref+', a perda se concentra entre <b>'+pos(t.conc.a)+'% e '+pos(t.conc.b)+'%</b> da volta ('+Math.round(100*t.conc.f)+'% dela).';\n"
        "if(t.share!==null&&t.share>=8)s+=' '+gl+' explica <b>'+t.share.toFixed(0)+'%</b> da diferenca de tempo entre as suas voltas.';\n"
        "s+='<br><span style=color:#7a847a>Medido nas voltas com perda:</span> '+diag(t)+'.';\n"
        "if(t.n<5)s+=' <i>(amostra pequena: '+t.n+' voltas)</i>';\n"
        "out.push('<li>'+s+'</li>');});\n"
        "if(!R.top.length){var tt=tiles.filter(function(t){return t.n<3;}).length;\n"
        "out.push('<li>Nenhum trecho com perda localizada acima de '+thr.toFixed(2)+'s alem do ritmo geral'+(tt?' (em '+tt+' trechos ha menos de 3 voltas validas para comparar)':'')+'. '+(R.clean.length<3?'Com so '+R.clean.length+' volta(s) limpa(s) nao ha base estatistica; veja o comparativo volta a volta abaixo.':'A diferenca entre as suas voltas limpas e de ritmo geral, nao de erro em um ponto.')+'</li>');}\n"
        "var idealT=R.tiles.reduce(function(a,t){return a+(t.n>=2?t.best:laps[R.best].tm[t.k].t);},0)*1000;\n"
        "var sect=sectorIdeal(),parts=[];\n"
        "if(sect)parts.push('somando o melhor de cada setor, a volta ideal e <b>'+fmt(sect)+'</b> ('+((R.bt-sect)/1000).toFixed(2)+'s abaixo da melhor)');\n"
        "if(R.clean.length>=3)parts.push('somando a melhor passagem de cada trecho: '+fmt(idealT)+' ('+((R.bt-idealT)/1000).toFixed(2)+'s) - mais otimista, o ruido de GPS infla esse numero');\n"
        "if(parts.length)out.push('<li><b>Teto de tempo:</b> '+parts.join('; ')+'.</li>');\n"
        "$('pri').innerHTML=out.join('');}\n"
        "function sectorIdeal(){\n"
        "var bs=[1e9,1e9,1e9],ok=false;\n"
        "R.clean.concat([R.best]).forEach(function(n){var o=laps[n];\n"
        "if(o.s1t&&o.s2t&&o.s2t>o.s1t&&o.time>o.s2t){ok=true;var s=[o.s1t,o.s2t-o.s1t,o.time-o.s2t];for(var i=0;i<3;i++)if(s[i]<bs[i])bs[i]=s[i];}});\n"
        "return ok?bs[0]+bs[1]+bs[2]:0;}\n"
        "function incidents(){\n"
        "var out=[],tiles=R.tiles;\n"
        "R.nums.forEach(function(n){var o=laps[n];\n"
        "o.inc.forEach(function(x){if((o.slow||o.inout)&&x.cls=='perda concentrada')return;\n"
        "var ts=tiles.filter(function(t){return x.a<t.b&&x.b>t.a;}).map(function(t){return t.name;});\n"
        "var where=ts.length?(ts.length>2?ts[0]+' a '+ts[ts.length-1]:ts.join(' e ')):'';\n"
        "var s='<b>Volta '+n+'</b>: '+x.cls+' - perdeu <b>'+x.loss.toFixed(1)+'s</b> entre '+pos(x.a)+'% e '+pos(x.b)+'% da volta'+(where?' ('+where+')':'')+'. Velocidade minima '+Math.round(x.vl)+' km/h (na melhor volta, no mesmo trecho: '+Math.round(x.vr)+')';\n"
        "if(x.stop>0.3)s+='; ~'+x.stop.toFixed(1)+'s abaixo de 5 km/h';\n"
        "out.push('<li>'+s+'.</li>');});\n"
        "if(o.cls=='saida/entrada do box (provavel)'||o.cls=='ritmo geral lento'){\n"
        "var tl=(o.time-R.bt)/1000;\n"
        "out.push('<li><b>Volta '+n+'</b>: '+o.cls+' - '+tl.toFixed(1)+'s mais lenta que a melhor, perda espalhada pela volta toda (velocidade media '+Math.round(o.vmed)+' km/h contra '+Math.round(laps[R.best].vmed)+'). Excluida das medias e das comparacoes por curva.</li>');}});\n"
        "if(!out.length)out.push('<li>Nenhum incidente detectado (parada, rodada ou perda concentrada acima de '+R.incThr.toFixed(1)+'s).</li>');\n"
        "$('inc').innerHTML=out.join('');}\n"
        "function variation(){\n"
        "var out=[],cl=R.clean;\n"
        "if(cl.length>=2){var ts=cl.map(function(n){return laps[n].time/1000;});\n"
        "var mn=Math.min.apply(null,ts),mx=Math.max.apply(null,ts);\n"
        "out.push('Voltas limpas: amplitude <b>'+(mx-mn).toFixed(2)+'s</b> (do melhor ao pior), desvio '+sdv(ts).toFixed(2)+'s.');}\n"
        "var sh=R.tiles.filter(function(t){return t.share!==null&&t.share>=10;}).sort(function(a,b){return b.share-a.share;}).slice(0,3);\n"
        "if(sh.length)out.push('Onde a diferenca entre as suas voltas nasce (parte da variacao do tempo de volta explicada por cada trecho): '+sh.map(function(t){return '<b>'+t.name+'</b> '+t.share.toFixed(0)+'%';}).join(', ')+'.');\n"
        "else if(cl.length>=5)out.push('A diferenca entre as suas voltas esta espalhada, sem um trecho dominante.');\n"
        "else out.push('Menos de 5 voltas limpas: nao ha base para dizer em qual trecho a variacao nasce.');\n"
        "if(cl.length>=6){var xs=cl.map(function(n){return n;}),ys=cl.map(function(n){return laps[n].time/1000;});\n"
        "var mx2=avg(xs),my=avg(ys),sxy=0,sxx=0,syy=0;\n"
        "for(var i=0;i<xs.length;i++){sxy+=(xs[i]-mx2)*(ys[i]-my);sxx+=(xs[i]-mx2)*(xs[i]-mx2);syy+=(ys[i]-my)*(ys[i]-my);}\n"
        "var sl=sxx?sxy/sxx:0,r2=(sxx&&syy)?sxy*sxy/(sxx*syy):0;R.slope=sl;R.r2=r2;\n"
        "if(r2>=0.3&&Math.abs(sl)*xs.length>=0.3)out.push('Tendencia ao longo da sessao: <b>'+(sl<0?'melhora de ':'piora de ')+Math.abs(sl).toFixed(2)+'s por volta</b> (R2 '+r2.toFixed(2)+')'+(sl>0?' - possivel queda de pneu/fadiga.':' - voce foi ganhando ritmo.'));\n"
        "else out.push('Sem tendencia clara de melhora ou piora ao longo da sessao (R2 '+r2.toFixed(2)+').');}\n"
        "$('vr').innerHTML=out.map(function(t){return '<li>'+t+'</li>';}).join('');}\n"
        "function segTimes(o){return (o.s1t&&o.s2t&&o.s2t>o.s1t&&o.time>o.s2t)?[o.s1t/1000,(o.s2t-o.s1t)/1000,(o.time-o.s2t)/1000]:null;}\n"
        "function table(){\n"
        "var nums=R.nums,h='<tr><th>Volta</th><th>Tempo</th><th>vs melhor</th><th>S1</th><th>S2</th><th>S3</th><th>Vel max</th><th>Vel media</th><th>Situacao</th></tr>';\n"
        "var mn=[1e9,1e9,1e9],mx=[0,0,0];\n"
        "R.clean.concat([R.best]).forEach(function(n){var s=segTimes(laps[n]);if(!s)return;for(var i=0;i<3;i++){if(s[i]<mn[i])mn[i]=s[i];if(s[i]>mx[i])mx[i]=s[i];}});\n"
        "nums.forEach(function(n){var o=laps[n],d=(o.time-R.bt)/1000,s=segTimes(o);\n"
        "var cs=function(i){if(!s)return '<td>-</td>';var v=s[i];\n"
        "var col=(o.cls=='ok'&&v==mn[i])?'#3ee07a':((o.cls=='ok'&&v==mx[i])?'#ff5a5a':'inherit');\n"
        "return '<td style=color:'+col+'>'+v.toFixed(3)+'</td>';};\n"
        "h+='<tr'+(n==R.best?' class=best':'')+(o.cls=='ok'?'':' style=opacity:.6')+'><td>'+n+'</td><td>'+fmt(o.time)+'</td><td>'+(n==R.best?'-':sg(d))+'</td>'+cs(0)+cs(1)+cs(2)+\n"
        "'<td>'+Math.round(o.vmax)+'</td><td>'+Math.round(o.vmed)+'</td><td>'+(n==R.best?'melhor':o.cls)+'</td></tr>';});\n"
        "$('tb').innerHTML=h;}\n"
        "function sectors(){\n"
        "var cl=R.clean.concat([R.best]).filter(function(n,i,a){return a.indexOf(n)==i;});\n"
        "var rows=[[],[],[]],ns=[];\n"
        "cl.forEach(function(n){var s=segTimes(laps[n]);if(!s)return;ns.push(n);for(var i=0;i<3;i++)rows[i].push(s[i]);});\n"
        "if(ns.length<2){$('sc').innerHTML='<tr><td>Dados de setor insuficientes (menos de 2 voltas limpas com S1/S2).</td></tr>';return;}\n"
        "var h='<tr><th>Setor</th><th>Melhor</th><th>Media</th><th>Pior</th><th>Amplitude</th><th>Volta do melhor</th></tr>',amp=[];\n"
        "for(var i=0;i<3;i++){var a=rows[i],mn=Math.min.apply(null,a),mx=Math.max.apply(null,a);amp.push(mx-mn);\n"
        "h+='<tr><td>S'+(i+1)+'</td><td>'+mn.toFixed(3)+'</td><td>'+avg(a).toFixed(3)+'</td><td>'+mx.toFixed(3)+'</td><td>'+(mx-mn).toFixed(2)+'s</td><td>'+ns[a.indexOf(mn)]+'</td></tr>';}\n"
        "var wi=amp.indexOf(Math.max.apply(null,amp));\n"
        "h+='<tr><td colspan=6 style=color:#7a847a>Setor mais irregular: <b style=color:#f5f8f5>S'+(wi+1)+'</b> (amplitude '+amp[wi].toFixed(2)+'s em '+ns.length+' voltas limpas).</td></tr>';\n"
        "$('sc').innerHTML=h;}\n"
        "function ctable(){\n"
        "var h='<tr><th>Trecho</th><th>Posicao</th><th>Melhor</th><th>Media</th><th>Perda media</th><th>Acima do ritmo</th><th>Entrada / Saida</th><th>V min (melhor | media)</th><th>Frenagem</th><th>Voltas c/ perda</th><th>Explica</th></tr>';\n"
        "R.tiles.forEach(function(t){\n"
        "var low=t.n<3,top=R.top&&R.top.indexOf(t)>=0;\n"
        "h+='<tr'+(low?' style=opacity:.55':'')+'><td>'+(top?'<b style=color:#ffd700>':'<b>')+t.name+'</b></td><td>'+span(t)+'</td><td>'+t.best.toFixed(2)+'s</td><td>'+t.tmean.toFixed(2)+'s</td><td>'+(low?'-':'<b style=color:'+(t.mean>=R.thr?'#ff5a5a':'inherit')+'>'+sg(t.mean)+'</b>')+'</td><td>'+(low?'-':'<b style=color:'+(t.exc>=R.thr?'#ff5a5a':'inherit')+'>'+sg(t.exc)+'</b>')+'</td>';\n"
        "if(t.type=='c'&&!low){h+='<td>'+sg(t.meanE)+' / '+sg(t.meanX)+'</td><td>'+Math.round(t.vminBest)+' | '+Math.round(t.vminAvg)+' km/h</td><td>'+(Math.abs(t.dbrk)<5?'igual':(t.dbrk<0?Math.round(-t.dbrk)+' m antes':Math.round(t.dbrk)+' m depois'))+'</td>';}\n"
        "else h+='<td>-</td><td>-</td><td>-</td>';\n"
        "h+='<td>'+(low?t.n+' (poucas)':t.nlose+' de '+t.n)+'</td><td>'+(t.share===null?'-':t.share.toFixed(0)+'%')+'</td></tr>';});\n"
        "$('ct').innerHTML=h;}\n"
        "function perlap(){\n"
        "var out=[],tiles=R.tiles;\n"
        "R.nums.forEach(function(n){if(n==R.best)return;var o=laps[n],d=(o.time-R.bt)/1000;\n"
        "var s='<b>Volta '+n+'</b> ('+fmt(o.time)+', '+sg(d)+')'+lapLabel(o)+': ';\n"
        "var ls=tiles.map(function(t){return {t:t,l:o.ok[t.k]?o.tm[t.k].t-t.best:0,ok:o.ok[t.k]};}).filter(function(x){return x.ok&&x.l>R.thr&&x.t.n>=2;}).sort(function(a,b){return b.l-a.l;});\n"
        "var ex=0,parts=[];\n"
        "o.inc.forEach(function(x){if((o.slow||o.inout)&&x.cls=='perda concentrada')return;ex+=x.loss;parts.push(x.loss.toFixed(1)+'s em incidente ('+x.cls+', '+pos(x.a)+'-'+pos(x.b)+'%)');});\n"
        "ls.slice(0,3).forEach(function(x){ex+=x.l;var dv=(x.t.type=='c'&&o.tm[x.t.k].vmin!==undefined)?o.tm[x.t.k].vmin-laps[x.t.ref].tm[x.t.k].vmin:null;\n"
        "parts.push(x.t.name+' '+sg(x.l)+(dv!==null&&dv<=-3?' (v min '+sk(dv)+' km/h)':''));});\n"
        "if(!parts.length)s+=o.cls=='ok'?'sem perda concentrada em nenhum trecho (diferenca espalhada).':'perda espalhada pela volta toda, sem trecho dominante.';\n"
        "else{s+=parts.join('; ')+'.';var rest=d-ex;if(rest>0.15&&o.cls!='saida/entrada do box (provavel)'&&o.cls!='ritmo geral lento')s+=' Restante espalhado: '+sg(rest)+'.';}\n"
        "out.push('<li>'+s+'</li>');});\n"
        "$('pl').innerHTML=out.join('');}\n"
        "function quality(){\n"
        "var out=[],nums=R.nums;\n"
        "var dv=nums.filter(function(n){return n!=R.best;}).map(function(n){return laps[n].dev;});\n"
        "var wd=dv.length?Math.max.apply(null,dv):0,wn=dv.length?nums.filter(function(n){return n!=R.best;})[dv.indexOf(wd)]:0;\n"
        "out.push('Alinhamento com a melhor volta: afastamento lateral medio '+(dv.length?avg(dv).toFixed(1):'0.0')+' m'+(wd>6?' <span style=color:#ff5a5a>(alto na volta '+wn+': '+wd.toFixed(1)+' m - caminho diferente ou GPS ruim; comparacoes por trecho dessa volta menos confiaveis)</span>':' (bom; maximo '+wd.toFixed(1)+' m na volta '+wn+')')+'.');\n"
        "var cov=nums.filter(function(n){return laps[n].rd[laps[n].rd.length-1]<R.Rf.last*0.95;});\n"
        "if(cov.length)out.push('<span style=color:#ff5a5a>Voltas '+cov.join(', ')+' nao cobrem o circuito inteiro (menos de 95% do tracado da melhor volta).</span>');\n"
        "var bad=nums.filter(function(n){return laps[n].bad>=5;}),gp=nums.filter(function(n){return laps[n].gaps>0;});\n"
        "out.push(bad.length?'<span style=color:#ff5a5a>Sinal GPS fraco (menos de 5 satelites) em mais de 5% das voltas '+bad.join(', ')+'.</span>':'Sinal GPS: sem trechos fracos relevantes (menos de 5 satelites em menos de 5% de cada volta).');\n"
        "if(gp.length)out.push('Falhas de amostragem (buracos de mais de 0,4s) nas voltas '+gp.map(function(n){return n+' ('+laps[n].gaps+')';}).join(', ')+'.');\n"
        "out.push('Metodo: cada volta e alinhada a sua melhor pela posicao na pista (nao pela fracao da distancia). Curvas sao detectadas pelos minimos de velocidade da melhor volta; o trecho de cada curva vai de um pico de velocidade ao seguinte. Perda = tempo do trecho menos o melhor tempo ja feito nele. Voltas com incidente, ritmo geral lento ou saida/entrada do box ficam fora das estatisticas por trecho. Sem acelerometro e sem freio: as causas sao inferidas so de velocidade e posicao.');\n"
        "if(R.relaxed)out.push('Poucas voltas limpas: o criterio de ritmo foi relaxado para 20% acima da melhor.');\n"
        "$('dq').innerHTML=out.map(function(t){return '<li>'+t+'</li>';}).join('');}\n"
        "function drawLaps(){\n"
        "var c=$('lt'),g=bg(c),nums=R.nums,ts=nums.map(function(n){return laps[n].time/1000;});\n"
        "var lo=Math.min.apply(null,ts),hi=Math.max.apply(null,ts),pad=(hi-lo)*0.12||0.5;lo-=pad;hi+=pad;\n"
        "var mL=70,W=c.width-mL-14,H=c.height-44;g.font='15px monospace';\n"
        "for(var i=0;i<=4;i++){var y=8+H-i*H/4,v=lo+(hi-lo)*i/4;\n"
        "g.strokeStyle='#262626';g.beginPath();g.moveTo(mL,y);g.lineTo(mL+W,y);g.stroke();\n"
        "g.fillStyle='#7a847a';g.fillText(v.toFixed(1)+'s',6,y+5);}\n"
        "function X(i){return nums.length>1?mL+i*W/(nums.length-1):mL+W/2;}\n"
        "function Y(v){return 8+H-(v-lo)/(hi-lo)*H;}\n"
        "if(R.clean.length>1){var a=avg(R.clean.map(function(n){return laps[n].time/1000;}));\n"
        "g.setLineDash([6,6]);g.strokeStyle='#5a5f58';g.beginPath();g.moveTo(mL,Y(a));g.lineTo(mL+W,Y(a));g.stroke();g.setLineDash([]);}\n"
        "g.strokeStyle='#3ec6e0';g.lineWidth=1.5;g.beginPath();\n"
        "nums.forEach(function(n,i){var x=X(i),y=Y(ts[i]);if(i)g.lineTo(x,y);else g.moveTo(x,y);});g.stroke();\n"
        "nums.forEach(function(n,i){var o=laps[n];g.fillStyle=n==R.best?'#3ee07a':(o.cls=='ok'?'#3ec6e0':(o.cls=='incidente'?'#ff5a5a':'#7a847a'));\n"
        "g.beginPath();g.arc(X(i),Y(ts[i]),n==R.best?8:5,0,7);g.fill();\n"
        "g.fillStyle='#7a847a';g.fillText(''+n,X(i)-6,c.height-8);});\n"
        "g.fillStyle='#7a847a';g.fillText('verde = melhor | ciano = limpa | vermelho = incidente | cinza = ritmo lento ou box | tracejado = media das limpas',mL+8,22);}\n"
        "function heatCol(l,thr,cap){\n"
        "if(l<thr*0.5)return 'rgb(22,48,28)';\n"
        "var t=Math.min(1,(l-thr*0.5)/(cap-thr*0.5)),r,gg,b,u;\n"
        "if(t<0.5){u=t/0.5;r=Math.round(40+215*u);gg=Math.round(70+130*u);b=Math.round(40-20*u);}\n"
        "else{u=(t-0.5)/0.5;r=255;gg=Math.round(200-140*u);b=20+Math.round(20*u);}\n"
        "return 'rgb('+r+','+gg+','+b+')';}\n"
        "function drawMap(){\n"
        "var c=$('mp'),g=bg(c),B=R.Rf,tiles=R.tiles;\n"
        "var x0=Math.min.apply(null,B.x),x1=Math.max.apply(null,B.x),y0=Math.min.apply(null,B.y),y1=Math.max.apply(null,B.y);\n"
        "var sx=(x1-x0)||1,sy=(y1-y0)||1,mg=40;\n"
        "var sc=Math.min((c.width-2*mg)/sx,(c.height-2*mg)/sy);\n"
        "var ox=mg+((c.width-2*mg)-sx*sc)/2,oy=mg+((c.height-2*mg)-sy*sc)/2;\n"
        "function X(x){return ox+(x-x0)*sc;}function Y(y){return oy+(y1-y)*sc;}\n"
        "var cap=Math.max(0.2,Math.max.apply(null,tiles.map(function(t){return t.n>=3?t.mean:0;})));\n"
        "g.lineWidth=9;g.lineCap='round';g.lineJoin='round';\n"
        "tiles.forEach(function(t){var i0=lb(B.d,t.a),i1=Math.min(lb(B.d,t.b),B.x.length-1);\n"
        "g.strokeStyle=t.n<3?'#3a3f3a':heatCol(t.mean,R.thr,cap);g.beginPath();\n"
        "for(var i=i0;i<=i1;i++){if(i==i0)g.moveTo(X(B.x[i]),Y(B.y[i]));else g.lineTo(X(B.x[i]),Y(B.y[i]));}g.stroke();});\n"
        "g.font='bold 18px sans-serif';\n"
        "tiles.forEach(function(t){if(t.type!='c')return;\n"
        "var i=Math.min(lb(B.d,t.apex),B.x.length-1),px=X(B.x[i]),py=Y(B.y[i]),top=R.top&&R.top.indexOf(t)>=0;\n"
        "g.fillStyle='#0d0f0d';g.beginPath();g.arc(px,py,15,0,7);g.fill();\n"
        "g.strokeStyle=top?'#ffd700':'#7a847a';g.lineWidth=top?3:2;g.beginPath();g.arc(px,py,15,0,7);g.stroke();\n"
        "g.fillStyle=top?'#ffd700':'#cfd6cf';var s=''+t.num;g.fillText(s,px-(s.length>1?10:5),py+6);});\n"
        "g.fillStyle='#f5f8f5';g.font='15px monospace';g.fillText('largada',X(B.x[0])+10,Y(B.y[0])-10);}\n"
        "var HM={};\n"
        "function drawHeat(){\n"
        "var c=$('hm'),nums=R.nums,tiles=R.tiles,rows=nums.length,ch=Math.max(22,Math.min(34,Math.floor(700/rows)));\n"
        "var top=40,mL=130;c.height=top+rows*ch+10;\n"
        "var g=bg(c),cw=(c.width-mL-10)/tiles.length;\n"
        "var pos2=[];nums.forEach(function(n){tiles.forEach(function(t){if(laps[n].ok[t.k]){var l=laps[n].tm[t.k].t-t.best;if(l>R.thr)pos2.push(l);}});});\n"
        "pos2.sort(function(a,b){return a-b;});\n"
        "var cap=Math.max(0.3,pos2.length?pos2[Math.floor(pos2.length*0.9)]:0.3);\n"
        "HM={top:top,mL:mL,cw:cw,ch:ch};\n"
        "g.font='14px monospace';\n"
        "tiles.forEach(function(t,k){g.fillStyle=t.type=='c'?'#f5f8f5':'#7a847a';g.fillText((t.type=='c'?'C':'R')+t.num,mL+k*cw+cw/2-10,top-12);});\n"
        "nums.forEach(function(n,r){var o=laps[n],y=top+r*ch;\n"
        "g.fillStyle=n==R.best?'#3ee07a':(o.cls=='ok'?'#f5f8f5':'#7a847a');\n"
        "g.fillText('V'+n+' '+(n==R.best?'melhor':sg((o.time-R.bt)/1000)),6,y+ch*0.65);\n"
        "tiles.forEach(function(t,k){var x=mL+k*cw;\n"
        "if(!o.ok[k]){g.fillStyle='#2a2e2a';g.fillRect(x,y,cw-1,ch-1);g.fillStyle='#5a5f58';g.fillText('x',x+cw/2-4,y+ch*0.65);return;}\n"
        "var l=o.tm[k].t-t.best;g.fillStyle=heatCol(l,R.thr,cap);g.fillRect(x,y,cw-1,ch-1);\n"
        "if(l>=R.thr&&cw>40){g.fillStyle=l/cap>0.6?'#fff':'#e8efe8';g.font='12px monospace';g.fillText(l.toFixed(2),x+4,y+ch*0.65);g.font='14px monospace';}});});\n"
        "HM.cap=cap;\n"
        "c.onmousemove=heatInfo;c.onclick=heatInfo;}\n"
        "function heatInfo(ev){\n"
        "var c=$('hm'),r=c.getBoundingClientRect(),sc=c.width/r.width;\n"
        "var x=(ev.clientX-r.left)*sc,y=(ev.clientY-r.top)*sc;\n"
        "var k=Math.floor((x-HM.mL)/HM.cw),i=Math.floor((y-HM.top)/HM.ch);\n"
        "if(k<0||k>=R.tiles.length||i<0||i>=R.nums.length){$('hi').textContent='';return;}\n"
        "var n=R.nums[i],o=laps[n],t=R.tiles[k],m=o.tm[k];\n"
        "if(!o.ok[k]){$('hi').innerHTML='Volta <b>'+n+'</b>, '+t.name+': excluida da comparacao ('+(o.slow||o.inout?o.cls:'incidente nesse trecho')+').';return;}\n"
        "var l=m.t-t.best,s='Volta <b>'+n+'</b>, <b>'+t.name+'</b> ('+span(t)+'): '+m.t.toFixed(2)+'s, '+(l<R.thr?'sem perda relevante':'<b>'+sg(l)+'</b> vs melhor passagem (volta '+t.ref+')')+' &middot; v min '+Math.round(m.vmin)+' km/h';\n"
        "if(t.type=='c')s+=', entrada '+Math.round(m.vin)+', saida '+Math.round(m.vout);\n"
        "$('hi').innerHTML=s;}\n"
        "load();\n"
        "</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * GET /evolucao - progresso ENTRE sessoes, agrupado por pista (o nome do
 * arquivo carrega o sufixo da pista). Best/media/consistencia por sessao
 * em ordem cronologica - responde "estou evoluindo nessa pista?".
 * --------------------------------------------------------------------- */
static esp_err_t evolution_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>KartBox - evolucao</title><style>"
        "body{margin:0 auto;padding:16px;max-width:960px;background:#0d0f0d;color:#f5f8f5;"
        "font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif}"
        "h1{font-size:18px;color:#3ee07a;border-bottom:2px solid #262626;padding-bottom:8px}"
        "a{color:#3ec6e0;text-decoration:none}"
        "select{background:#141714;color:#f5f8f5;border:1px solid #262626;border-radius:8px;"
        "padding:8px;font-size:14px;margin:0 8px 12px 4px}"
        "canvas{width:100%;background:#141714;border:1px solid #262626;border-radius:8px}"
        "table{width:100%;border-collapse:collapse;font-size:14px;margin-top:12px}"
        "th,td{padding:7px 10px;border-bottom:1px solid #262626;text-align:left}"
        "th{color:#7a847a;font-size:12px;text-transform:uppercase}"
        "tr.best td{color:#3ee07a}"
        "#err{color:#ff5a5a}.lg{font-size:13px;color:#7a847a;margin:8px 0}"
        "</style></head><body>"
        "<p><a href=\"/\">&larr; sessoes</a></p>"
        "<h1>Evolucao entre sessoes</h1>"
        "<div>Pista <select id=\"trk\"></select> <span class=\"lg\" id=\"cnt\"></span></div>"
        "<canvas id=\"ev\" width=\"1200\" height=\"420\"></canvas>"
        "<p class=\"lg\"><span style=\"color:#3ee07a\">verde = best da sessao</span> &middot; "
        "<span style=\"color:#3ec6e0\">ciano = media das voltas limpas</span></p>"
        "<div id=\"sum\" class=\"lg\"></div>"
        "<table id=\"tb\"></table>"
        "<p id=\"err\"></p>"
        "<script>\n"
        "var groups={};\n"
        "function parseName(f){\n"
        "var m=f.match(/(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})(?:_(.+))?\\.csv$/);\n"
        "if(!m)return {track:'(sem data)',label:f,ts:0};\n"
        "return {track:(m[7]||'(sem pista)').replace(/_/g,' '),\n"
        "label:m[3]+'/'+m[2]+' '+m[4]+':'+m[5],\n"
        "ts:+(m[1]+m[2]+m[3]+m[4]+m[5]+m[6])};\n"
        "}\n"
        "fetch('/api/sessions').then(function(r){return r.text();}).then(function(txt){\n"
        "txt.trim().split(/\\n/).forEach(function(f){if(!f)return;\n"
        "var p=parseName(f);\n"
        "if(!groups[p.track])groups[p.track]=[];\n"
        "groups[p.track].push({f:f,label:p.label,ts:p.ts});});\n"
        "var names=Object.keys(groups).sort();\n"
        "if(!names.length){err.textContent='nenhuma sessao no cartao';return;}\n"
        "names.forEach(function(n){trk.add(new Option(n+' ('+groups[n].length+')',n));});\n"
        "trk.onchange=loadTrack;loadTrack();\n"
        "}).catch(function(){err.textContent='falha ao listar sessoes';});\n"
        "function loadTrack(){\n"
        "err.textContent='';sum.innerHTML='';tb.innerHTML='';\n"
        "var ss=groups[trk.value].slice().sort(function(a,b){return a.ts-b.ts;}).slice(-25);\n"
        "cnt.textContent='carregando '+ss.length+' sessoes...';\n"
        "Promise.all(ss.map(function(s){\n"
        "return fetch('/api/laps?file='+encodeURIComponent(s.f)).then(function(r){return r.text();})\n"
        ".then(function(t){\n"
        "var times=[];t.trim().split(/\\n/).forEach(function(l){\n"
        "var c=l.split(',');if(c.length>=2&&+c[1]>0)times.push(+c[1]/1000);});\n"
        "if(!times.length)return null;\n"
        "var best=Math.min.apply(null,times);\n"
        "var clean=times.filter(function(x){return x<=best*1.10;});\n"
        "var m=clean.reduce(function(a,b){return a+b;},0)/clean.length;\n"
        "var sd=Math.sqrt(clean.reduce(function(a,b){return a+(b-m)*(b-m);},0)/clean.length);\n"
        "return {label:s.label,laps:times.length,best:best,mean:m,sd:sd};\n"
        "});\n"
        "})).then(function(rows){\n"
        "rows=rows.filter(function(r){return r;});\n"
        "cnt.textContent=rows.length+' sessoes com voltas';\n"
        "if(!rows.length){err.textContent='nenhuma volta fechada nessa pista';return;}\n"
        "render(rows);\n"
        "}).catch(function(){err.textContent='falha ao carregar sessoes';});\n"
        "}\n"
        "function render(rows){\n"
        "var ctx=ev.getContext('2d');ctx.clearRect(0,0,ev.width,ev.height);\n"
        "var vals=[];rows.forEach(function(r){vals.push(r.best);vals.push(r.mean);});\n"
        "var lo=Math.min.apply(null,vals),hi=Math.max.apply(null,vals);\n"
        "if(hi-lo<0.5){var c=(hi+lo)/2;lo=c-0.25;hi=c+0.25;}\n"
        "var mL=64,W=ev.width-mL-16,H=ev.height-52;\n"
        "ctx.font='15px monospace';\n"
        "for(var g=0;g<=5;g++){var y=10+H-g*H/5;\n"
        "ctx.strokeStyle='#262626';ctx.beginPath();ctx.moveTo(mL,y);ctx.lineTo(mL+W,y);ctx.stroke();\n"
        "ctx.fillStyle='#7a847a';ctx.fillText((lo+(hi-lo)*g/5).toFixed(2),6,y+5);}\n"
        "function X(i){return rows.length>1?mL+i*W/(rows.length-1):mL+W/2;}\n"
        "function Y(v){return 10+H-(v-lo)/(hi-lo)*H;}\n"
        "function series(key,col){ctx.strokeStyle=col;ctx.lineWidth=2;ctx.beginPath();\n"
        "rows.forEach(function(r,i){var x=X(i),y=Y(r[key]);\n"
        "if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y);});ctx.stroke();\n"
        "ctx.fillStyle=col;rows.forEach(function(r,i){\n"
        "ctx.beginPath();ctx.arc(X(i),Y(r[key]),4,0,7);ctx.fill();});}\n"
        "series('mean','#3ec6e0');series('best','#3ee07a');\n"
        "ctx.fillStyle='#7a847a';\n"
        "rows.forEach(function(r,i){\n"
        "if(rows.length<=8||i==0||i==rows.length-1||i%Math.ceil(rows.length/8)==0)\n"
        "ctx.fillText(r.label,X(i)-34,ev.height-8);});\n"
        "ctx.fillText('tempo de volta (s)',mL+8,24);\n"
        /* tabela + resumo */
        "var bi=0;rows.forEach(function(r,i){if(r.best<rows[bi].best)bi=i;});\n"
        "var h='<tr><th>Sessao</th><th>Voltas</th><th>Best</th><th>Media</th><th>Consistencia</th></tr>';\n"
        "rows.forEach(function(r,i){\n"
        "h+='<tr'+(i==bi?' class=best':'')+'><td>'+r.label+'</td><td>'+r.laps+'</td><td>'+\n"
        "r.best.toFixed(3)+'s</td><td>'+r.mean.toFixed(3)+'s</td><td>&plusmn;'+r.sd.toFixed(3)+'s</td></tr>';});\n"
        "tb.innerHTML=h;\n"
        "if(rows.length>=2){\n"
        "var d=rows[0].best-rows[rows.length-1].best;\n"
        "sum.innerHTML='Da primeira pra ultima sessao: best '+\n"
        "(d>0?'<b style=color:#3ee07a>melhorou '+d.toFixed(3)+'s</b>':\n"
        "'<b style=color:#ff5a5a>piorou '+(-d).toFixed(3)+'s</b>')+\n"
        "' &middot; recorde da pista: <b>'+rows[bi].best.toFixed(3)+'s</b> ('+rows[bi].label+')';\n"
        "}\n"
        "}\n"
        "</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * A-GPS (u-blox AssistNow Online) - dois caminhos pro mesmo fim
 * (injetar UBX-MGA no modulo via gps_inject_agps):
 *
 *   POST /agps     - corpo = arquivo .ubx baixado manualmente do servico
 *                    (funciona em qualquer modo, ate no AP proprio).
 *   GET  /agps_dl  - o FIRMWARE baixa direto da u-blox e injeta (fluxo
 *                    AssistNow/ZTP abaixo). Exige modo Cliente com internet.
 *                    Tambem roda sozinho ao conectar no WiFi.
 * --------------------------------------------------------------------- */
#define AGPS_MAX_BYTES (256 * 1024)

static esp_err_t agps_post_handler(httpd_req_t *req)
{
    touch_activity();

    if (req->content_len == 0 || req->content_len > AGPS_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tamanho invalido (max 256KB)");
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
        return ESP_FAIL;
    }

    int remaining = (int)req->content_len;
    uint8_t *p = buf;
    while (remaining > 0) {
        int r = httpd_req_recv(req, (char *)p, remaining);
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrompido");
            return ESP_FAIL;
        }
        p += r;
        remaining -= r;
    }

    bool ok = gps_inject_agps(buf, req->content_len);
    free(buf);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao injetar no modulo");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok - dados A-GPS injetados no modulo");
    return ESP_OK;
}

/* ---- AssistNow (Thingstream ZTP) -------------------------------------
 * 1) Registro (1x por modulo): le UBX-SEC-UNIQID + UBX-MON-VER do GPS e
 *    faz POST em api.thingstream.io/ztp/assistnow/credentials com o token
 *    do perfil. Resposta traz o "chipcode", guardado no NVS.
 * 2) Download: GET GetAssistNowData.ashx?chipcode=..&gnss=..&data=.. devolve
 *    UBX-MGA cru, injetado direto no modulo.
 * Roda sozinho ao conectar no WiFi (modo Cliente) e em GET /agps_dl. */
#define AGPS_ZTP_URL      "https://api.thingstream.io/ztp/assistnow/credentials"
#define AGPS_DATA_URL     "https://assistnow.services.u-blox.com/GetAssistNowData.ashx"
#define AGPS_MIN_GAP_MS   (30 * 60 * 1000)   /* nao rebaixa antes de 30 min */

static volatile bool    s_agps_busy = false;
static volatile int64_t s_agps_last_ok_ms = 0;

/* Le o corpo inteiro de uma resposta ja aberta (headers lidos). */
static int agps_read_all(esp_http_client_handle_t cli, uint8_t *buf, int cap)
{
    int total = 0;
    while (total < cap) {
        int r = esp_http_client_read(cli, (char *)buf + total, cap - total);
        if (r <= 0) break;
        total += r;
    }
    return total;
}

/* Registra o modulo e grava o chipcode. msg recebe motivo em caso de erro. */
static bool agps_register(char *msg, size_t msgsz)
{
    char uid[48], ver[1100];
    if (!gps_read_ztp_info(uid, sizeof uid, ver, sizeof ver)) {
        snprintf(msg, msgsz, "GPS nao respondeu (UBX)");
        return false;
    }
    size_t bsz = strlen(AGPS_ZTP_TOKEN_DEFAULT) + strlen(uid) + strlen(ver) + 128;
    char *body = malloc(bsz);
    char *resp = malloc(1024);
    if (!body || !resp) { free(body); free(resp); snprintf(msg, msgsz, "sem memoria"); return false; }
    int bl = snprintf(body, bsz,
        "{\"token\":\"%s\",\"messages\":{\"UBX-SEC-UNIQID\":\"%s\",\"UBX-MON-VER\":\"%s\"}}",
        AGPS_ZTP_TOKEN_DEFAULT, uid, ver);

    esp_http_client_config_t cfg = { .url = AGPS_ZTP_URL, .method = HTTP_METHOD_POST,
        .timeout_ms = 20000, .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    bool ok = false;
    if (!cli) { snprintf(msg, msgsz, "http init"); goto out; }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    if (esp_http_client_open(cli, bl) != ESP_OK) {
        snprintf(msg, msgsz, "sem internet/TLS"); esp_http_client_cleanup(cli); goto out;
    }
    esp_http_client_write(cli, body, bl);
    esp_http_client_fetch_headers(cli);
    int st = esp_http_client_get_status_code(cli);
    int n = agps_read_all(cli, (uint8_t *)resp, 1023);
    resp[n > 0 ? n : 0] = '\0';
    esp_http_client_cleanup(cli);
    char chip[SETTINGS_AGPS_CHIP_MAX];
    if (st == 200 && kb_json_str(resp, "chipcode", chip, sizeof chip)) {
        settings_set_agps_chip(chip);
        ESP_LOGI(TAG, "A-GPS: modulo registrado, chipcode salvo");
        ok = true;
    } else {
        ESP_LOGW(TAG, "A-GPS: registro falhou HTTP %d: %.120s", st, resp);
        snprintf(msg, msgsz, "registro recusado (HTTP %d)", st);
    }
out:
    free(body); free(resp);
    return ok;
}

/* Baixa + injeta. Devolve bytes injetados (>0) ou <0: -1 rede, -2 chipcode
 * recusado (401/403: precisa registrar de novo), -3 resposta nao-UBX, -4 UART. */
static int agps_fetch_inject(const char *chip, const char *data, char *msg, size_t msgsz)
{
    char enc[SETTINGS_AGPS_CHIP_MAX * 3];
    if (!kb_url_encode(chip, enc, sizeof enc)) { snprintf(msg, msgsz, "chipcode longo"); return -2; }
    char url[512];
    snprintf(url, sizeof url, AGPS_DATA_URL "?chipcode=%s&gnss=gps,gal,bds,glo&data=%s", enc, data);

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 25000, .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { snprintf(msg, msgsz, "http init"); return -1; }
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        esp_http_client_cleanup(cli); snprintf(msg, msgsz, "sem internet/TLS"); return -1;
    }
    esp_http_client_fetch_headers(cli);
    int st = esp_http_client_get_status_code(cli);
    if (st == 401 || st == 403) {
        esp_http_client_cleanup(cli); snprintf(msg, msgsz, "chipcode recusado (HTTP %d)", st); return -2;
    }
    if (st != 200) {
        esp_http_client_cleanup(cli); snprintf(msg, msgsz, "u-blox HTTP %d", st); return -1;
    }
    uint8_t *buf = heap_caps_malloc(AGPS_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(cli); snprintf(msg, msgsz, "sem memoria"); return -1; }
    int total = agps_read_all(cli, buf, AGPS_MAX_BYTES);
    esp_http_client_cleanup(cli);
    if (total < 8 || buf[0] != 0xB5 || buf[1] != 0x62) {
        /* servidor responde texto de erro (ex: tipo nao autorizado) com 200 as vezes */
        ESP_LOGW(TAG, "A-GPS: resposta nao e' UBX (%d B): %.80s", total, total > 0 ? (char *)buf : "");
        free(buf); snprintf(msg, msgsz, "resposta invalida"); return -3;
    }
    ESP_LOGI(TAG, "A-GPS: %d bytes baixados (%s), injetando...", total, data);
    bool ok = gps_inject_agps(buf, (size_t)total);
    free(buf);
    if (!ok) { snprintf(msg, msgsz, "falha ao injetar"); return -4; }
    return total;
}

/* Fluxo completo: registra se preciso, baixa, injeta. true = injetou. */
static bool agps_sync(char *msg, size_t msgsz)
{
    if (s_agps_busy) { snprintf(msg, msgsz, "ja em andamento"); return false; }
    s_agps_busy = true;
    bool ok = false;
    msg[0] = '\0';

    if (!settings_get_agps_chip()[0] && !agps_register(msg, msgsz)) goto done;

    /* Tenta orbitas "live" + almanaque + hora; se o plano nao autorizar,
     * cai pras preditivas de 1 dia. Cada tentativa conta 1 req (limite 300/mes). */
    static const char *const kSets[] = { "utime,ulorb_l1,ualm", "utime,uporb_1,ualm" };
    bool reregistered = false;
    size_t i = 0;
    while (i < sizeof kSets / sizeof kSets[0]) {
        char chip[SETTINGS_AGPS_CHIP_MAX];
        strncpy(chip, settings_get_agps_chip(), sizeof chip - 1); chip[sizeof chip - 1] = '\0';
        int r = agps_fetch_inject(chip, kSets[i], msg, msgsz);
        if (r > 0) { ok = true; break; }
        if (r == -2) {                       /* chipcode invalido: re-registra 1x e repete o set */
            if (reregistered) break;
            reregistered = true;
            settings_set_agps_chip("");
            if (!agps_register(msg, msgsz)) break;
        } else if (r == -3) {
            i++;                             /* tipo nao autorizado/resposta texto: tenta o proximo set */
        } else {
            break;                           /* rede/UART: outro set nao ajuda */
        }
    }
    if (ok) s_agps_last_ok_ms = esp_timer_get_time() / 1000;
done:
    s_agps_busy = false;
    return ok;
}

/* Disparada ao pegar IP no modo Cliente. Espera a rede assentar, evita
 * repetir em < 30 min (reconexoes) e avisa o resultado na tela. */
static void agps_auto_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(4000));
    int64_t now = esp_timer_get_time() / 1000;
    if (s_agps_last_ok_ms && now - s_agps_last_ok_ms < AGPS_MIN_GAP_MS) { vTaskDelete(NULL); return; }
    char msg[64];
    if (agps_sync(msg, sizeof msg)) ui_show_toast("A-GPS atualizado", 3000);
    else { ESP_LOGW(TAG, "A-GPS auto: %s", msg); ui_show_toast("A-GPS falhou - ver log", 3000); }
    vTaskDelete(NULL);
}

static esp_err_t agps_download_handler(httpd_req_t *req)
{
    touch_activity();
    char msg[64];
    if (!agps_sync(msg, sizeof msg)) {
        char e[96];
        snprintf(e, sizeof e, "A-GPS falhou: %s", msg);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, e);
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok - A-GPS injetado no modulo GPS");
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Editor de pista (celular) - GET /editor serve o editor.html.
 * O arquivo vai EMBUTIDO no firmware (~25KB, main/CMakeLists.txt
 * target_add_binary_data), entao funciona sem nada no cartao. Um
 * "editor.html" na raiz do SD e' IGNORADO (versao antiga esquecida no
 * cartao ja serviu editor desatualizado).
 *
 * ATENCAO: servido por http:// simples, o navegador do celular bloqueia a
 * Geolocation (exige contexto seguro) - dados moveis nao mudam isso. O
 * proprio editor mostra um aviso explicando; marcar no mapa e sincronizar
 * funcionam normal. Pra usar o GPS do celular, abrir o arquivo local.
 *
 * /api/tracks e /api/track trocam pistas em binario CRU, byte a byte
 * identico ao arquivo .trk (== struct track_config_t) - nao tem parser
 * nenhum dos dois lados, so memcpy. O editor.html monta esse mesmo
 * buffer no browser (DataView little-endian) pra exportar pro SD local
 * OU mandar direto pro kartbox por aqui.
 *
 * CORS liberado (Access-Control-Allow-Origin: *) porque o fluxo tipico
 * e abrir o editor.html com internet normal (pra Geolocation funcionar -
 * exige "secure context", https/localhost/file://, que o kartbox em
 * http:// simples NAO satisfaz) e so trocar pra rede do kartbox (AP
 * proprio OU, se estiver em modo STA, a mesma rede que o celular ja
 * esta) na hora de sincronizar.
 * --------------------------------------------------------------------- */
static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

/* editor.html embutido (CMake gera estes simbolos; TEXT => terminado em NUL) */
extern const char editor_html_start[] asm("_binary_editor_html_start");

static esp_err_t editor_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    /* Sempre a copia embutida no firmware: assim o editor nunca fica
     * desatualizado por causa de um editor.html antigo esquecido no SD. */
    return httpd_resp_send(req, editor_html_start, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/gps - posicao atual do modulo GPS do kartbox, em JSON. Usado pelo
 * botao "Usar GPS do kartbox" do editor (nao depende da Geolocation do
 * navegador, que exige https). Sem fix: {"fix":false,"sats":N}. */
static esp_err_t api_gps_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    gps_sample_t s;
    gps_get_latest(&s);

    char buf[160];
    if (s.fix_valid) {
        snprintf(buf, sizeof(buf),
                 "{\"fix\":true,\"sats\":%u,\"lat\":%.7f,\"lon\":%.7f,\"speed\":%.1f,\"heading\":%.1f}",
                 (unsigned)s.satellites, s.lat, s.lon, (double)s.speed_kmh, (double)s.heading_deg);
    } else {
        snprintf(buf, sizeof(buf), "{\"fix\":false,\"sats\":%u}", (unsigned)s.satellites);
    }
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t tracks_list_get_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");

    char names[TRACK_LIST_MAX][TRACK_NAME_MAX];
    int n = track_manager_list(names, TRACK_LIST_MAX);
    for (int i = 0; i < n; i++) {
        httpd_resp_sendstr_chunk(req, names[i]);
        httpd_resp_sendstr_chunk(req, "\n");
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t track_get_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);

    char query[96], name[TRACK_NAME_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "name", name, sizeof(name));
    }
    if (name[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou ?name=");
        return ESP_FAIL;
    }

    track_config_t cfg;
    if (!track_manager_load(name, &cfg)) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, (const char *)&cfg, sizeof(cfg));
    return ESP_OK;
}

static esp_err_t track_post_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);

    if (req->content_len != sizeof(track_config_t)) {
        ESP_LOGW(TAG, "POST /api/track: tamanho invalido (%d, esperado %d)",
                 (int)req->content_len, (int)sizeof(track_config_t));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tamanho invalido");
        return ESP_FAIL;
    }

    track_config_t cfg;
    int received = 0;
    while (received < (int)sizeof(cfg)) {
        int r = httpd_req_recv(req, ((char *)&cfg) + received, sizeof(cfg) - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao receber corpo");
            return ESP_FAIL;
        }
        received += r;
    }

    if (cfg.magic != TRACK_MAGIC || cfg.name[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "magic ou nome invalido");
        return ESP_FAIL;
    }
    cfg.name[TRACK_NAME_MAX - 1] = '\0';

    if (!track_manager_save(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao salvar no SD");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t track_delete_handler(httpd_req_t *req)
{
    touch_activity();
    add_cors_headers(req);

    char query[96], name[TRACK_NAME_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "name", name, sizeof(name));
    }
    if (name[0] == '\0' || !track_manager_delete(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nao foi possivel apagar");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* Preflight CORS - navegador manda OPTIONS antes de POST/DELETE com
 * Content-Type != form/text simples (nosso caso: application/octet-stream). */
static esp_err_t api_options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /delete?file=NOME - apaga um CSV de sessao do cartao. Mesma
 * sanitizacao do download (so basename, rejeita "..") + exige extensao
 * .csv, pra so poder apagar arquivo de sessao, nunca outra coisa no SD.
 * A confirmacao ("tem certeza?") e feita no navegador (ver kbDel). */
static esp_err_t delete_session_post_handler(httpd_req_t *req)
{
    touch_activity();

    char query[160];
    char filename[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "file", filename, sizeof(filename));
    }

    const char *base = filename;
    size_t blen = strlen(base);
    if (!web_filename_ok(base) ||
        blen < 5 || strcmp(base + blen - 4, ".csv") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nome de arquivo invalido");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, base);

    if (remove(path) != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sessao apagada: %s", base);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Explorer do SD (GET /arquivos + /api/fs*). Pagina embutida no firmware
 * (explorer.html). Todo caminho vem da URL e passa por kb_fs_path_ok()
 * (sem "..", sem "//", sem controle) e e' SEMPRE prefixado com o ponto de
 * montagem - nao ha como sair do /sdcard.
 * --------------------------------------------------------------------- */
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

extern const char explorer_html_start[] asm("_binary_explorer_html_start");
extern const char logview_html_start[]  asm("_binary_logview_html_start");

#define FS_PATH_MAX   (160)
#define FS_UPLOAD_MAX (512u * 1024u * 1024u)

static esp_err_t explorer_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, explorer_html_start, HTTPD_RESP_USE_STRLEN);
}

/* Le ?p= validado e monta o caminho real em full. false = ja respondeu 400/503. */
static bool fs_get_path(httpd_req_t *req, char *rel, size_t relsz, char *full, size_t fullsz)
{
    char query[FS_PATH_MAX * 3];
    rel[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_param(query, "p", rel, relsz);
    }
    if (!rel[0]) { rel[0] = '/'; rel[1] = '\0'; }
    if (!kb_fs_path_ok(rel, relsz)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return false;
    }
    if (!sd_logger_is_mounted()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "cartao SD nao montado");
        return false;
    }
    size_t rl = strlen(rel);
    if (rl > 1 && rel[rl - 1] == '/') rel[rl - 1] = '\0';   /* barra final some */
    if (rel[1] == '\0') snprintf(full, fullsz, "%s", SD_MOUNT_POINT);
    else                snprintf(full, fullsz, "%s%s", SD_MOUNT_POINT, rel);
    return true;
}

/* nome seguro pra ir dentro de JSON "..." (descarta aspas/barra/controle) */
static bool fs_name_json_ok(const char *n)
{
    for (const unsigned char *c = (const unsigned char *)n; *c; c++) {
        if (*c < 0x20 || *c == 0x7F || *c == '"' || *c == '\\') return false;
    }
    return true;
}

static esp_err_t fs_list_handler(httpd_req_t *req)
{
    touch_activity();
    char rel[FS_PATH_MAX], full[FS_PATH_MAX + 16];
    if (!fs_get_path(req, rel, sizeof rel, full, sizeof full)) return ESP_FAIL;

    DIR *d = opendir(full);
    if (!d) { httpd_resp_send_404(req); return ESP_FAIL; }

    uint64_t total = 0, freeb = 0;
    sd_get_card_info(&total, &freeb);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[96];
    snprintf(head, sizeof head, "{\"total\":%llu,\"free\":%llu,\"items\":[",
             (unsigned long long)total, (unsigned long long)freeb);
    httpd_resp_sendstr_chunk(req, head);

    bool first = true;
    struct dirent *e;
    char item[FS_PATH_MAX + 96], p2[FS_PATH_MAX + 300];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!fs_name_json_ok(e->d_name)) continue;
        snprintf(p2, sizeof p2, "%s/%s", full, e->d_name);
        struct stat st;
        if (stat(p2, &st) != 0) continue;
        int n = snprintf(item, sizeof item, "%s{\"n\":\"%s\",\"d\":%d,\"s\":%lu,\"t\":%lld}",
                         first ? "" : ",", e->d_name, S_ISDIR(st.st_mode) ? 1 : 0,
                         (unsigned long)(S_ISDIR(st.st_mode) ? 0 : st.st_size), (long long)st.st_mtime);
        if (n <= 0 || n >= (int)sizeof item) continue;
        first = false;
        if (httpd_resp_sendstr_chunk(req, item) != ESP_OK) { closedir(d); return ESP_FAIL; }
    }
    closedir(d);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t fs_get_handler(httpd_req_t *req)
{
    touch_activity();
    char rel[FS_PATH_MAX], full[FS_PATH_MAX + 16];
    if (!fs_get_path(req, rel, sizeof rel, full, sizeof full)) return ESP_FAIL;

    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) { httpd_resp_send_404(req); return ESP_FAIL; }
    FILE *f = fopen(full, "rb");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

    char bn[64], hdr[112];
    kb_fs_basename(rel, bn, sizeof bn);
    snprintf(hdr, sizeof hdr, "attachment; filename=\"%s\"", bn);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    char *buf = malloc(4096);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria"); return ESP_FAIL; }
    size_t n;
    esp_err_t res = ESP_OK;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { res = ESP_FAIL; break; }
        touch_activity();
    }
    free(buf);
    fclose(f);
    if (res == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    else httpd_resp_sendstr_chunk(req, NULL);
    return res;
}

static esp_err_t fs_put_handler(httpd_req_t *req)
{
    touch_activity();
    char rel[FS_PATH_MAX], full[FS_PATH_MAX + 16];
    if (!fs_get_path(req, rel, sizeof rel, full, sizeof full)) return ESP_FAIL;
    if (rel[1] == '\0') { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido"); return ESP_FAIL; }
    if ((size_t)req->content_len > FS_UPLOAD_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "arquivo grande demais (max 512MB)");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ja existe uma pasta com esse nome");
        return ESP_FAIL;
    }
    FILE *f = fopen(full, "wb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nao consegui criar o arquivo"); return ESP_FAIL; }
    char *buf = malloc(4096);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria"); return ESP_FAIL; }

    size_t remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(buf, 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
        remaining -= (size_t)r;
        touch_activity();
    }
    free(buf);
    fclose(f);
    if (!ok) {
        remove(full);   /* nao deixa arquivo pela metade */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "envio interrompido ou cartao cheio");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SD upload: %s (%u bytes)", rel, (unsigned)req->content_len);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t fs_rm_handler(httpd_req_t *req)
{
    touch_activity();
    char rel[FS_PATH_MAX], full[FS_PATH_MAX + 16];
    if (!fs_get_path(req, rel, sizeof rel, full, sizeof full)) return ESP_FAIL;
    if (rel[1] == '\0') { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nao da pra apagar a raiz"); return ESP_FAIL; }

    struct stat st;
    if (stat(full, &st) != 0) { httpd_resp_send_404(req); return ESP_FAIL; }
    int rc = S_ISDIR(st.st_mode) ? rmdir(full) : remove(full);
    if (rc != 0) {
        if (errno == ENOTEMPTY || errno == EEXIST) httpd_resp_set_status(req, "409 Conflict");
        else httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "nao foi possivel apagar");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SD apagado: %s", rel);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t fs_mkdir_handler(httpd_req_t *req)
{
    touch_activity();
    char rel[FS_PATH_MAX], full[FS_PATH_MAX + 16];
    if (!fs_get_path(req, rel, sizeof rel, full, sizeof full)) return ESP_FAIL;
    if (rel[1] == '\0') { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido"); return ESP_FAIL; }
    if (mkdir(full, 0775) != 0 && errno != EEXIST) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nao consegui criar a pasta");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Log do sistema via WiFi (GET /log + /api/log). wifi_export_log_start()
 * (chamada no comeco do app_main) engancha o vprintf do ESP_LOG: tudo que
 * iria pra serial tambem entra num anel de 32KB. A pagina /log consulta
 * /api/log?since=N a cada 1s e recebe so o que e' novo (cabecalho X-Next
 * = proximo offset). Nao substitui a serial no boot muito cedo (antes do
 * app_main) nem apos o anel dar a volta (~20 min de log normal).
 * --------------------------------------------------------------------- */
#include <stdarg.h>
#if WIFI_LOG_ENABLED
#define LOG_RING_SIZE (32 * 1024)
#else
#define LOG_RING_SIZE (64)        /* desligado: anel minimo, nada e capturado */
#endif
#define LOG_LINE_MAX  (512)

static char              s_log_ring[LOG_RING_SIZE];
static volatile uint32_t s_log_total = 0;            /* bytes escritos desde o boot (monotonico) */
static portMUX_TYPE      s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t    s_log_prev = NULL;

static int wifi_log_vprintf(const char *fmt, va_list ap)
{
    char tmp[LOG_LINE_MAX];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n >= (int)sizeof tmp) { n = (int)sizeof tmp - 1; tmp[n - 1] = '\n'; }
        portENTER_CRITICAL(&s_log_mux);
        uint32_t t = s_log_total;
        for (int i = 0; i < n; i++) s_log_ring[(t + (uint32_t)i) % LOG_RING_SIZE] = tmp[i];
        s_log_total = t + (uint32_t)n;
        portEXIT_CRITICAL(&s_log_mux);
    }
    return s_log_prev ? s_log_prev(fmt, ap) : vprintf(fmt, ap);
}

void wifi_export_log_start(void)
{
#if !WIFI_LOG_ENABLED
    return;                                    /* captura de log desligada (config.h) */
#endif
    if (s_log_prev) return;                    /* ja instalado */
    s_log_prev = esp_log_set_vprintf(wifi_log_vprintf);
    if (!s_log_prev) s_log_prev = vprintf;
}

static esp_err_t logview_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, logview_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_log_handler(httpd_req_t *req)
{
    touch_activity();
    char query[48], sv[16] = {0};
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "since", sv, sizeof sv) == ESP_OK) {
        since = (uint32_t)strtoul(sv, NULL, 10);
    }
    portENTER_CRITICAL(&s_log_mux);
    uint32_t total = s_log_total;
    portEXIT_CRITICAL(&s_log_mux);

    if (since > total) since = 0;                           /* reiniciou */
    if (total - since > LOG_RING_SIZE) since = total - LOG_RING_SIZE;   /* perdeu o comeco */

    char nx[16];
    snprintf(nx, sizeof nx, "%lu", (unsigned long)total);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Next", nx);

    char chunk[512];
    uint32_t pos = since;
    while (pos < total) {
        uint32_t n = total - pos;
        if (n > sizeof chunk) n = sizeof chunk;
        portENTER_CRITICAL(&s_log_mux);
        for (uint32_t i = 0; i < n; i++) chunk[i] = s_log_ring[(pos + i) % LOG_RING_SIZE];
        portEXIT_CRITICAL(&s_log_mux);
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) return ESP_FAIL;
        pos += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------------------------------------------------------------------
 * GET /api/coredump - baixa o coredump pendente DIRETO da flash (sem passar
 * pelo SD). Existe porque o SD as vezes da' timeout e o coredump fica
 * preso na flash; sem USB a unica saida e' o WiFi. Analisar no PC:
 *   idf.py coredump-info --core coredump.bin   (ou espcoredump.py
 *   info_corefile -c coredump.bin build/kartbox.elf)
 * --------------------------------------------------------------------- */
static esp_err_t coredump_get_handler(httpd_req_t *req)
{
    touch_activity();
    size_t addr = 0, size = 0;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "sem coredump pendente na flash");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    uint8_t *buf = malloc(1024);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria"); return ESP_FAIL; }
    size_t off = 0;
    esp_err_t res = ESP_OK;
    while (off < size) {
        size_t n = (size - off) > 1024 ? 1024 : (size - off);
        if (esp_partition_read(part, off, buf, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK) { res = ESP_FAIL; break; }
        off += n;
        touch_activity();
    }
    free(buf);
    if (res == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    else httpd_resp_sendstr_chunk(req, NULL);
    return res;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 32; /* default (8) nao sobra pra APIs + OTA + A-GPS + analise + evolucao */
    config.stack_size = 8192; /* handlers com snprintf/JSON/fopen estouram os 4096 default */

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start falhou (%s)", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t index_uri     = { .uri = "/",           .method = HTTP_GET,     .handler = index_get_handler };
    const httpd_uri_t download_uri  = { .uri = "/download",   .method = HTTP_GET,     .handler = download_get_handler };
    const httpd_uri_t upload_uri    = { .uri = "/upload",     .method = HTTP_POST,    .handler = upload_post_handler };
    const httpd_uri_t delete_uri    = { .uri = "/delete",     .method = HTTP_POST,    .handler = delete_session_post_handler };
    const httpd_uri_t editor_uri    = { .uri = "/editor",     .method = HTTP_GET,     .handler = editor_get_handler };
    const httpd_uri_t tracks_uri    = { .uri = "/api/tracks", .method = HTTP_GET,     .handler = tracks_list_get_handler };
    const httpd_uri_t track_get_uri = { .uri = "/api/track",  .method = HTTP_GET,     .handler = track_get_handler };
    const httpd_uri_t track_post_uri= { .uri = "/api/track",  .method = HTTP_POST,    .handler = track_post_handler };
    const httpd_uri_t track_del_uri = { .uri = "/api/track",  .method = HTTP_DELETE,  .handler = track_delete_handler };
    const httpd_uri_t track_opt_uri = { .uri = "/api/track",  .method = HTTP_OPTIONS, .handler = api_options_handler };
    const httpd_uri_t ota_uri       = { .uri = "/ota",        .method = HTTP_POST,    .handler = ota_post_handler };
    const httpd_uri_t agps_uri      = { .uri = "/agps",       .method = HTTP_POST,    .handler = agps_post_handler };
    const httpd_uri_t agps_dl_uri   = { .uri = "/agps_dl",    .method = HTTP_GET,     .handler = agps_download_handler };
    const httpd_uri_t analysis_uri  = { .uri = "/analise",    .method = HTTP_GET,     .handler = analysis_get_handler };
    const httpd_uri_t report_uri    = { .uri = "/relatorio",  .method = HTTP_GET,     .handler = report_get_handler };
    const httpd_uri_t evol_uri      = { .uri = "/evolucao",     .method = HTTP_GET,   .handler = evolution_get_handler };
    const httpd_uri_t api_sess_uri  = { .uri = "/api/sessions", .method = HTTP_GET,   .handler = api_sessions_handler };
    const httpd_uri_t api_laps_uri  = { .uri = "/api/laps",     .method = HTTP_GET,   .handler = api_laps_handler };
    const httpd_uri_t api_gps_uri   = { .uri = "/api/gps",      .method = HTTP_GET,   .handler = api_gps_handler };
    const httpd_uri_t fs_page_uri   = { .uri = "/arquivos",     .method = HTTP_GET,   .handler = explorer_get_handler };
    const httpd_uri_t fs_list_uri   = { .uri = "/api/fs",       .method = HTTP_GET,   .handler = fs_list_handler };
    const httpd_uri_t fs_get_uri    = { .uri = "/api/fs/get",   .method = HTTP_GET,   .handler = fs_get_handler };
    const httpd_uri_t fs_put_uri    = { .uri = "/api/fs/put",   .method = HTTP_POST,  .handler = fs_put_handler };
    const httpd_uri_t fs_rm_uri     = { .uri = "/api/fs/rm",    .method = HTTP_POST,  .handler = fs_rm_handler };
    const httpd_uri_t log_page_uri  = { .uri = "/log",          .method = HTTP_GET,   .handler = logview_get_handler };
    const httpd_uri_t log_api_uri   = { .uri = "/api/log",      .method = HTTP_GET,   .handler = api_log_handler };
    const httpd_uri_t core_uri      = { .uri = "/api/coredump", .method = HTTP_GET,   .handler = coredump_get_handler };
    const httpd_uri_t fs_mkdir_uri  = { .uri = "/api/fs/mkdir", .method = HTTP_POST,  .handler = fs_mkdir_handler };
    httpd_register_uri_handler(s_httpd, &ota_uri);
    httpd_register_uri_handler(s_httpd, &agps_uri);
    httpd_register_uri_handler(s_httpd, &agps_dl_uri);
    httpd_register_uri_handler(s_httpd, &analysis_uri);
    httpd_register_uri_handler(s_httpd, &report_uri);
    httpd_register_uri_handler(s_httpd, &evol_uri);
    httpd_register_uri_handler(s_httpd, &api_sess_uri);
    httpd_register_uri_handler(s_httpd, &api_laps_uri);
    httpd_register_uri_handler(s_httpd, &api_gps_uri);
    httpd_register_uri_handler(s_httpd, &fs_page_uri);
    httpd_register_uri_handler(s_httpd, &fs_list_uri);
    httpd_register_uri_handler(s_httpd, &fs_get_uri);
    httpd_register_uri_handler(s_httpd, &fs_put_uri);
    httpd_register_uri_handler(s_httpd, &fs_rm_uri);
    httpd_register_uri_handler(s_httpd, &fs_mkdir_uri);
    httpd_register_uri_handler(s_httpd, &core_uri);
    httpd_register_uri_handler(s_httpd, &log_page_uri);
    httpd_register_uri_handler(s_httpd, &log_api_uri);
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &download_uri);
    httpd_register_uri_handler(s_httpd, &upload_uri);
    httpd_register_uri_handler(s_httpd, &delete_uri);
    httpd_register_uri_handler(s_httpd, &editor_uri);
    httpd_register_uri_handler(s_httpd, &tracks_uri);
    httpd_register_uri_handler(s_httpd, &track_get_uri);
    httpd_register_uri_handler(s_httpd, &track_post_uri);
    httpd_register_uri_handler(s_httpd, &track_del_uri);
    httpd_register_uri_handler(s_httpd, &track_opt_uri);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Desliga sozinho depois de tempo parado - assim o WiFi (caro em RAM/
 * energia) so fica no ar quando alguem de fato esta baixando algo.
 * --------------------------------------------------------------------- */
static void idle_check_cb(void *arg)
{
    (void)arg;
    if (!s_active) return;
    int64_t elapsed_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;
    if (elapsed_ms >= WIFI_AP_IDLE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "WiFi ocioso ha %lld ms, desligando", (long long)elapsed_ms);
        wifi_export_stop();
        app_event_post(APP_EVT_WIFI_TIMEOUT, EVT_SRC_INTERNAL);
    }
}

static void start_idle_timer(void)
{
    const esp_timer_create_args_t targs = {
        .callback = idle_check_cb,
        .name = "wifi_idle",
    };
    esp_timer_create(&targs, &s_idle_timer);
    esp_timer_start_periodic(s_idle_timer, 30LL * 1000 * 1000); /* checa a cada 30s */
}

static void stop_idle_timer(void)
{
    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
        esp_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
    }
}

/* ---------------------------------------------------------------------
 * Modo cliente (STA) - handler de eventos + conexao.
 *
 * Variacao do exemplo classico "wifi station" do ESP-IDF: la, STA_START
 * ja dispara esp_wifi_connect() sozinho. Aqui NAO - wifi_export_scan()
 * tambem precisa subir o radio em modo STA (sem querer conectar em nada,
 * so escanear), e se o handler conectasse automaticamente todo STA_START
 * ele tentaria logar na ultima rede configurada no meio do scan. Quem
 * decide conectar e' explicitamente start_sta(), chamando
 * esp_wifi_connect() direto apos o esp_wifi_start(). Dai em diante,
 * STA_DISCONNECTED tenta de novo ate WIFI_STA_MAX_RETRY vezes (senha
 * errada ou rede fora de alcance nao trava pra sempre); IP_EVENT_STA_
 * GOT_IP e o sinal de sucesso de verdade (associar sem DHCP nao serve
 * pra nada aqui).
 * --------------------------------------------------------------------- */
static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Interface STA de fato pronta (ver comentario em
         * WIFI_STA_START_TIMEOUT_MS) - libera quem esta esperando pra
         * escanear/conectar. NAO chama esp_wifi_connect() aqui de
         * proposito (ver comentario grande abaixo): scan tambem usa
         * STA_START e nao quer conectar em nada. */
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_STA_STARTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_scanning) {
            /* Disconnect disparado pelo proprio scan - nao reconecta, so
             * ignora (ver comentario em s_scanning). */
        } else if (s_active && s_mode == WIFI_EXPORT_MODE_STA) {
            /* ja estava rodando e caiu (rede saiu do alcance, roteador
             * reiniciou etc) - nao fica tentando pra sempre sozinho,
             * so reflete o estado; usuario decide reativar pelo menu. */
            s_sta_state = WIFI_STA_STATE_FAILED;
            s_sta_ip[0] = '\0';
            ESP_LOGW(TAG, "STA: conexao caiu (\"%s\")", s_sta_ssid);
        } else if (s_sta_retry_count < WIFI_STA_MAX_RETRY) {
            s_sta_retry_count++;
            ESP_LOGW(TAG, "STA: falha ao conectar em \"%s\", tentativa %d/%d",
                      s_sta_ssid, s_sta_retry_count, WIFI_STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            s_sta_state = WIFI_STA_STATE_FAILED;
            if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_STA_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_retry_count = 0;
        s_sta_state = WIFI_STA_STATE_CONNECTED;
        ESP_LOGI(TAG, "STA: conectado em \"%s\", IP=%s", s_sta_ssid, s_sta_ip);
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
        /* A-GPS automatico: baixa efemerides/hora e injeta no GPS (cold start rapido). */
        if (!s_agps_busy) xTaskCreate(agps_auto_task, "agps_auto", 12288, NULL, 4, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        /* Le os resultados JA' AQUI, o mais perto possivel do evento real
         * (ver comentario grande em s_scan_records acima) - nao deixa pra
         * wifi_export_scan() ler depois, que perdia a corrida contra o
         * coprocessador derrubando a STA logo apos o scan nesse setup
         * hosted. */
        wifi_event_sta_scan_done_t *scan_done = (wifi_event_sta_scan_done_t *)event_data;
        s_scan_got = 0;
        if (scan_done && scan_done->status != 0) {
            /* status != 0 = scan abortado/falhou no C6 - ap_num aqui e'
             * lixo, nao adianta ler. Loga pra diferenciar de "rodou e
             * nao achou nada". */
            ESP_LOGW(TAG, "SCAN_DONE com status=%u (scan abortado no coprocessador)",
                     (unsigned)scan_done->status);
        } else {
            uint16_t num = scan_done ? (uint16_t)scan_done->number : 0;
            if (num == 0) esp_wifi_scan_get_ap_num(&num); /* fallback se o evento nao trouxe */
            if (num > WIFI_SCAN_MAX_RESULTS) num = WIFI_SCAN_MAX_RESULTS;
            s_scan_got = num;
            if (num > 0) {
                esp_wifi_scan_get_ap_records(&s_scan_got, s_scan_records);
            }
        }
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    }
}

/* Garante esp_wifi_init() + netifs AP e STA + handlers registrados, uma
 * unica vez (idempotente). Criar os dois netifs de saida (nao so o do
 * modo atual) evita ter que recriar netif toda vez que o usuario troca
 * de modo AP<->STA. */
static esp_err_t ensure_wifi_driver_ready(void)
{
    if (s_wifi_inited) return ESP_OK;

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init falhou (%s) - confira esp_wifi_remote/esp_hosted no idf_component.yml",
                  esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_sta_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_sta_event_handler, NULL, NULL);

    /* Event group unico compartilhado por scan e conexao STA. Criado
     * AQUI (antes de qualquer esp_wifi_start()) pra garantir que o bit
     * WIFI_STA_STARTED do handler nunca se perca por corrida. */
    if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();

    /* Fixa o country/canal-set - sem isso o C6 pode assumir um dominio
     * restrito e nao varrer alguns canais. ieee80211d_enabled=true deixa
     * o beacon do AP sobrescrever se a regiao real for outra. */
    wifi_country_t country = {
        .cc = "BR", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);

    s_wifi_inited = true;
    return ESP_OK;
}

static bool start_ap(void)
{
    /* Bug real encontrado em log: esp_read_mac(..., ESP_MAC_WIFI_SOFTAP)
     * chamado ANTES do esp_wifi_init() dava "E system_api: mac type is
     * incorrect (not found)". Nesse board o radio wifi mora no
     * coprocessador C6 (esp_hosted via SDIO), entao a MAC da interface
     * SOFTAP so existe depois que o driver (que aqui e so um proxy RPC
     * pro C6) foi inicializado. esp_wifi_get_mac() pergunta pro driver
     * de verdade (ja com o C6 respondendo), da certo mesmo em setup
     * hosted. */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s%02X%02X", WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "esp_wifi_get_mac falhou - mantendo SSID generico \"%s\"", s_ssid);
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.ap.ssid, s_ssid, sizeof(wifi_cfg.ap.ssid) - 1);
    wifi_cfg.ap.ssid_len = strlen(s_ssid);
    strncpy((char *)wifi_cfg.ap.password, s_password, sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.channel = WIFI_AP_CHANNEL;
    wifi_cfg.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao subir AP (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "AP \"%s\" ativo - export em http://192.168.4.1/ ou http://%s.local/",
              s_ssid, WIFI_MDNS_HOSTNAME);
    return true;
}

static bool start_sta(void)
{
    if (s_sta_ssid[0] == '\0') {
        ESP_LOGE(TAG, "STA: nenhuma rede configurada - escolha/escaneie uma rede na aba Config antes de ativar");
        s_sta_state = WIFI_STA_STATE_FAILED;
        return false;
    }

    /* s_wifi_event_group criado em ensure_wifi_driver_ready(). Limpa
     * tambem STARTED pra esperar a interface subir antes de conectar. */
    xEventGroupClearBits(s_wifi_event_group,
        WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT | WIFI_STA_STARTED_BIT);
    s_sta_retry_count = 0;
    s_sta_state = WIFI_STA_STATE_CONNECTING;
    s_sta_ip[0] = '\0';

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, s_sta_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, s_sta_password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = s_sta_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA: falha ao subir radio (%s)", esp_err_to_name(err));
        s_sta_state = WIFI_STA_STATE_FAILED;
        return false;
    }

    /* Espera a interface subir (WIFI_EVENT_STA_START) antes de conectar -
     * mesma corrida assincrona do hosted descrita no scan. */
    EventBits_t sb = xEventGroupWaitBits(s_wifi_event_group, WIFI_STA_STARTED_BIT,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_STA_START_TIMEOUT_MS));
    if (!(sb & WIFI_STA_STARTED_BIT)) {
        ESP_LOGW(TAG, "STA: interface nao subiu em %dms, tentando conectar mesmo assim", WIFI_STA_START_TIMEOUT_MS);
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA: falha ao subir radio/conectar (%s)", esp_err_to_name(err));
        s_sta_state = WIFI_STA_STATE_FAILED;
        return false;
    }

    /* Espera o resultado assincrono (conectou/desistiu) do handler de
     * eventos, com timeout - ver comentario grande acima de
     * wifi_sta_event_handler() sobre o esp_wifi_connect() ser explicito
     * aqui em vez de automatico no STA_START. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_STA_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA: export em http://%s/ ou http://%s.local/", s_sta_ip, WIFI_MDNS_HOSTNAME);
        return true;
    }

    ESP_LOGE(TAG, "STA: nao conectou em \"%s\" dentro de %ds (timeout, fora de alcance ou senha errada)",
              s_sta_ssid, WIFI_STA_CONNECT_TIMEOUT_MS / 1000);
    s_sta_state = WIFI_STA_STATE_FAILED;
    esp_wifi_stop();
    return false;
}

/* ---------------------------------------------------------------------
 * API publica
 * --------------------------------------------------------------------- */
void wifi_export_init(void)
{
    static bool netif_ready = false;
    if (!netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        netif_ready = true;
    }

    /* SSID provisorio (generico) so pra nao ficar vazio antes do primeiro
     * wifi_export_start() em modo AP - a definitiva (com sufixo de MAC
     * de verdade) so e montada dentro de start_ap(). */
    snprintf(s_ssid, sizeof(s_ssid), "%s????", WIFI_AP_SSID_PREFIX);

    /* mDNS - "kartbox.local" funciona tanto conectado no AP proprio
     * quanto (modo STA) na rede do usuario, independente do IP mudar.
     * Nao depende de nenhum netif estar ativo ainda pra inicializar. */
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(WIFI_MDNS_HOSTNAME);
        mdns_instance_name_set("KartBox v2");
        ESP_LOGI(TAG, "mDNS pronto - http://%s.local/", WIFI_MDNS_HOSTNAME);
    } else {
        ESP_LOGW(TAG, "mdns_init falhou (%s) - so vai dar pra acessar por IP", esp_err_to_name(mdns_err));
    }

    ESP_LOGI(TAG, "WiFi export pronto (sob demanda)");
}

bool wifi_export_start(void)
{
    if (s_active) return false;

    if (ensure_wifi_driver_ready() != ESP_OK) return false;

    bool ok = (s_mode == WIFI_EXPORT_MODE_AP) ? start_ap() : start_sta();
    if (!ok) return false;

    if (start_http_server() != ESP_OK) {
        if (s_mode == WIFI_EXPORT_MODE_AP) esp_wifi_stop();
        else esp_wifi_disconnect();
        return false;
    }

    s_last_activity_us = esp_timer_get_time();
    start_idle_timer();
    s_active = true;
    return true;
}

void wifi_export_stop(void)
{
    if (!s_active) return;
    stop_idle_timer();
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_mode == WIFI_EXPORT_MODE_STA) {
        esp_wifi_disconnect();
        s_sta_state = WIFI_STA_STATE_IDLE;
        s_sta_ip[0] = '\0';
    }
    esp_wifi_stop();
    s_active = false;
    ESP_LOGI(TAG, "WiFi export desligado");
}

bool wifi_export_is_active(void)
{
    return s_active;
}

const char *wifi_export_get_ssid(void)
{
    return s_ssid;
}

const char *wifi_export_get_password(void)
{
    return s_password;
}

void wifi_export_set_password(const char *password)
{
    if (!password || strlen(password) < 8) return; /* WPA2 exige minimo 8 chars */
    strncpy(s_password, password, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';
}

void wifi_export_set_mode(wifi_export_mode_t mode)
{
    if (s_active) {
        ESP_LOGW(TAG, "set_mode: wifi ja ativo - desligue antes de trocar de modo");
        return;
    }
    s_mode = mode;
}

wifi_export_mode_t wifi_export_get_mode(void)
{
    return s_mode;
}

int wifi_export_scan(char out_ssids[][WIFI_SCAN_SSID_MAX], int max_results)
{
    if (s_active) {
        ESP_LOGW(TAG, "scan: desligue o wifi export antes de escanear (precisa do radio livre)");
        return 0;
    }
    if (ensure_wifi_driver_ready() != ESP_OK) return 0;

    s_scanning = true; /* segura o auto-reconnect do handler ate o fim (ver s_scanning) */

    /* s_wifi_event_group ja' foi criado em ensure_wifi_driver_ready()
     * (antes de qualquer start), entao o bit STA_STARTED do handler nao
     * se perde. Limpa os bits que vamos esperar antes de subir o radio. */
    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT | WIFI_STA_STARTED_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);

    /* CRITICO: sobe a STA com config VAZIA (ssid=""). Sem isso, o slave
     * C6 auto-conecta na ultima rede STA salva assim que start() sobe (dois
     * "wifi station started" no log), e esp_wifi_scan_start() volta
     * ESP_ERR_WIFI_STATE = "STA is connecting, scan are not allowed". Sem
     * alvo, nao ha conexao pra bloquear o scan. As credenciais de verdade
     * ficam no nosso settings/NVS e sao reaplicadas em start_sta() na hora
     * de conectar - nada se perde aqui. */
    if (err == ESP_OK) {
        wifi_config_t empty_cfg = {0};
        err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
    }
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: falha ao subir radio em modo STA (%s)", esp_err_to_name(err));
        s_scanning = false;
        return 0;
    }

    /* ESPERA a interface STA subir de verdade (WIFI_EVENT_STA_START)
     * antes de escanear. esp_wifi_start() e' assincrono no hosted (RPC
     * pro C6); escanear antes disso fazia esp_wifi_scan_start() voltar
     * ESP_ERR_WIFI_STATE ou disparar um scan vazio (number=0). Essa era a
     * causa n\\u00ba1 do "scan nunca acha nada". */
    EventBits_t sb = xEventGroupWaitBits(s_wifi_event_group, WIFI_STA_STARTED_BIT,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_STA_START_TIMEOUT_MS));
    if (!(sb & WIFI_STA_STARTED_BIT)) {
        ESP_LOGW(TAG, "scan: STA nao subiu em %dms (WIFI_EVENT_STA_START), abortando", WIFI_STA_START_TIMEOUT_MS);
        esp_wifi_stop();
        s_scanning = false;
        return 0;
    }

    /* Bug real encontrado: nesse board o radio wifi e' remoto (esp_hosted
     * via SDIO pro coprocessador C6 - "H_SDIO_DRV" no log de boot). Com
     * block=true, esp_wifi_scan_start() usa a espera BLOQUEANTE padrao do
     * esp-idf, que nesse setup hosted nao sincroniza direito com o scan
     * de verdade rodando no C6 - a chamada volta quase na hora, antes do
     * coprocessador terminar, e esp_wifi_scan_get_ap_num() sempre da 0.
     * Fix: scan NAO bloqueante + espera explicita pelo evento real
     * WIFI_EVENT_SCAN_DONE (ver wifi_sta_event_handler) com timeout.
     *
     * scan_cfg com dwell explicito (default 0 as vezes nao captura nada
     * no hosted) - active scan em todos os canais. */
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = {
            .active = { .min = WIFI_SCAN_DWELL_MIN_MS, .max = WIFI_SCAN_DWELL_MAX_MS },
        },
    };

    /* Aborta qualquer tentativa de conexao residual antes de escanear -
     * defensivo contra o estado "connecting" que faz o scan voltar
     * ESP_ERR_WIFI_STATE. Ignora o retorno (da erro benigno se ja estava
     * desconectado). */
    esp_wifi_disconnect();

    /* Retry curto: mesmo com config vazia + disconnect, o slave pode
     * ficar alguns ms num estado transitorio que ainda recusa o scan
     * (ESP_ERR_WIFI_STATE). Tenta ate WIFI_SCAN_START_RETRIES vezes com
     * uma pausa curta antes de desistir. */
    err = ESP_FAIL;
    for (int attempt = 0; attempt < WIFI_SCAN_START_RETRIES; attempt++) {
        err = esp_wifi_scan_start(&scan_cfg, false /* nao bloqueia - espera o evento abaixo */);
        if (err == ESP_OK) break;
        if (err != ESP_ERR_WIFI_STATE) break; /* outro erro nao melhora com retry */
        ESP_LOGW(TAG, "scan: STA ainda em estado transitorio (tentativa %d/%d), aguardando...",
                 attempt + 1, WIFI_SCAN_START_RETRIES);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_START_RETRY_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_scan_start falhou (%s)", esp_err_to_name(err));
        esp_wifi_stop();
        s_scanning = false;
        return 0;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT,
                                            pdTRUE /* limpa o bit ao sair */, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS));
    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        ESP_LOGW(TAG, "scan: timeout (%dms) esperando WIFI_EVENT_SCAN_DONE", WIFI_SCAN_TIMEOUT_MS);
        esp_wifi_stop();
        s_scanning = false;
        return 0;
    }

    /* s_scan_records/s_scan_got ja' foram preenchidos DENTRO do handler
     * de evento (wifi_sta_event_handler), no instante mais proximo
     * possivel do WIFI_EVENT_SCAN_DONE - ver comentario grande junto da
     * declaracao de s_scan_records. Chamar esp_wifi_scan_get_ap_records()
     * de novo aqui (from scratch, minutos... digo, milissegundos depois,
     * de outra task) e' exatamente o que perdia a corrida contra o
     * coprocessador derrubando a STA logo apos o scan nesse setup hosted -
     * so' le o que ja' foi capturado. */
    int count = 0;
    for (int i = 0; i < s_scan_got && count < max_results; i++) {
        const char *ssid = (const char *)s_scan_records[i].ssid;
        if (ssid[0] == '\0') continue; /* rede oculta, nada pra mostrar */

        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out_ssids[j], ssid) == 0) { dup = true; break; }
        }
        if (dup) continue; /* mesma rede em mais de um AP/canal (mesh, repetidor) */

        strncpy(out_ssids[count], ssid, WIFI_SCAN_SSID_MAX - 1);
        out_ssids[count][WIFI_SCAN_SSID_MAX - 1] = '\0';
        count++;
    }

    esp_wifi_stop(); /* radio so ligou pra esse scan - wifi_export_start() decide quando ligar de vez */
    s_scanning = false;

    ESP_LOGI(TAG, "scan: %d rede(s) encontrada(s)", count);
    return count;
}

void wifi_export_set_sta_credentials(const char *ssid, const char *password)
{
    if (ssid) {
        strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
        s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    }
    if (password) {
        strncpy(s_sta_password, password, sizeof(s_sta_password) - 1);
        s_sta_password[sizeof(s_sta_password) - 1] = '\0';
    }
}

const char *wifi_export_get_sta_ssid(void)
{
    return s_sta_ssid;
}

wifi_sta_state_t wifi_export_get_sta_state(void)
{
    return s_sta_state;
}

void wifi_export_get_sta_ip(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (s_sta_state == WIFI_STA_STATE_CONNECTED) {
        strncpy(out, s_sta_ip, out_size - 1);
        out[out_size - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

const char *wifi_export_get_mdns_hostname(void)
{
    return WIFI_MDNS_HOSTNAME;
}
