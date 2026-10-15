/*
 * wifi_export.c - ver wifi_export.h pro contexto.
 */
#include "wifi_export.h"
#include "config.h"
#include "app_events.h"
#include "track_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
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
        "qualquer outro (ex: editor.html) &rarr; raiz do cartao</p>"
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
            httpd_resp_sendstr_chunk(req, "<li><b>");
            httpd_resp_sendstr_chunk(req, de->d_name);
            /* <small> com data-file: preenchido no navegador (kbMeta) com a
             * data legivel e a pista, derivadas do nome do arquivo. */
            httpd_resp_sendstr_chunk(req, "</b><small class=\"meta\" data-file=\"");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "\"></small> <a href=\"/download?file=");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "\">CSV</a> <button onclick=\"kbExp('");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "','gpx')\">GPX</button> <button onclick=\"kbExp('");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "','vbo')\">VBO</button> <button onclick=\"kbExp('");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "','rc')\">RaceChrono</button> <a href=\"/analise?file=");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "\">Analise</a> <button class=\"del\" onclick=\"kbDel('");
            httpd_resp_sendstr_chunk(req, de->d_name);
            httpd_resp_sendstr_chunk(req, "')\">Excluir</button></li>");
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
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }

    /* SO o nome do arquivo. Rejeita qualquer coisa que tente sair da
     * pasta de sessoes. */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (base[0] == '\0' || strstr(base, "..") != NULL) {
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
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (base[0] == '\0' || strstr(base, "..") != NULL) {
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
        fwrite(buf, 1, r, f);
        remaining -= r;
    }
    fclose(f);

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
        "<p class=\"noprint\"><a href=\"/\">&larr; sessoes</a></p>"
        "<h1 id=\"hd\">Analise</h1>"
        "<div class=\"noprint\">Volta A <select id=\"selA\"></select> "
        "Volta B <select id=\"selB\"></select> "
        "<button onclick=\"window.print()\" style=\"float:right\">Imprimir relatorio</button></div>"
        "<div id=\"stats\" class=\"lg\" style=\"font-size:15px;color:#f5f8f5\"></div>"
        "<canvas id=\"cv\" width=\"1200\" height=\"460\"></canvas>"
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
        "hd.textContent='Analise - '+f;\n"
        "fetch('/download?file='+encodeURIComponent(f)).then(function(r){return r.text();}).then(function(csv){\n"
        "var L=csv.trim().split(/\\r?\\n/);var prev=null;var ox=null,oy=null,okx=1;\n"
        "for(var i=1;i<L.length;i++){var c=L[i].split(',');if(c.length<8)continue;\n"
        "var lap=+c[6];if(lap<1)continue;\n"
        "var lat=+c[1],lon=+c[2],v=+c[3],t=+c[7],sat=+c[5]||0;\n"
        "if(lat==0&&lon==0)continue;\n"
        "if(ox===null){ox=lon;oy=lat;okx=111320*Math.cos(lat*Math.PI/180);}\n"
        "if(!laps[lap])laps[lap]={d:[],v:[],x:[],y:[],sat:[],last:0,time:0,vmax:0,vsum:0,vcnt:0,s1d:0,s2d:0};\n"
        "var o=laps[lap];\n"
        "if(prev&&prev.lap==lap){var kx=111320*Math.cos(lat*Math.PI/180);\n"
        "var dx=(lon-prev.lon)*kx,dy=(lat-prev.lat)*111320;\n"
        "o.last+=Math.sqrt(dx*dx+dy*dy);}\n"
        "o.d.push(o.last);o.v.push(v);o.time=t;\n"
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
        "drawMap();\n"
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
        httpd_query_key_value(query, "file", fname, sizeof(fname));
    }
    if (!fname[0] || strstr(fname, "..")) {
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
        ")).then(function(rows){\n"
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
 *   GET  /agps_dl  - o FIRMWARE baixa direto da u-blox e injeta. Exige
 *                    modo Cliente numa rede com internet (ex: roteamento
 *                    do celular). ?token=XXX na primeira vez (salvo em
 *                    NVS); depois pode chamar sem token.
 *
 * Token: gratis em thingstream.io (Location Services > AssistNow).
 * HTTP simples (sem TLS) - o servico u-blox atende nas duas portas e os
 * dados de efemerides sao publicos/nao-sensiveis; poupa o handshake TLS.
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

static esp_err_t agps_download_handler(httpd_req_t *req)
{
    touch_activity();

    char query[128];
    char token[SETTINGS_AGPS_TOKEN_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "token", token, sizeof(token));
    }
    if (token[0]) {
        settings_set_agps_token(token); /* informou = salva pros proximos */
    } else {
        strncpy(token, settings_get_agps_token(), sizeof(token) - 1);
    }
    if (!token[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "sem token AssistNow - crie gratis em thingstream.io e informe no campo");
        return ESP_FAIL;
    }

    /* eph+alm+aux das 4 constelacoes que o MAX-M10 usa. A resposta ja
     * comeca com UBX-MGA-INI-TIME_UTC (hora do servidor) - o modulo
     * ganha efemerides + hora de uma vez, que e' o que corta o cold
     * start de minutos pra segundos.
     * HTTPS obrigatorio: o servico NAO atende http simples (testado em
     * campo - recusa/redireciona, e aqui a resposta e' lida na mao, sem
     * seguir redirect). crt_bundle do IDF valida o certificado. */
    char url[256];
    snprintf(url, sizeof(url),
             "https://online-live1.services.u-blox.com/GetOnlineData.ashx"
             "?token=%s;gnss=gps,gal,bds,glo;datatype=eph,alm,aux", token);

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao criar cliente http");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        /* open() cobre DNS+TCP+TLS - falha aqui pode ser tanto rede
         * (sem internet / modo AP) quanto handshake TLS (ex: falta de
         * RAM interna pro AES de hardware - ver sdkconfig.defaults). O
         * log serial (esp-tls/esp-aes) diz qual dos dois foi. */
        ESP_LOGE(TAG, "A-GPS: esp_http_client_open falhou (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "falha ao conectar no servico u-blox - sem internet (modo Cliente?) ou TLS falhou (ver log serial)");
        return ESP_FAIL;
    }
    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        esp_http_client_cleanup(cli);
        ESP_LOGW(TAG, "A-GPS: servico u-blox respondeu %d", status);
        char emsg[96];
        snprintf(emsg, sizeof(emsg),
                 "servico u-blox respondeu HTTP %d (403=token errado/expirado)", status);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, emsg);
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(AGPS_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        esp_http_client_cleanup(cli);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
        return ESP_FAIL;
    }
    int total = 0;
    while (total < AGPS_MAX_BYTES) {
        int r = esp_http_client_read(cli, (char *)buf + total, AGPS_MAX_BYTES - total);
        if (r <= 0) break;
        total += r;
    }
    esp_http_client_cleanup(cli);

    if (total <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "resposta vazia do servico u-blox");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "A-GPS: %d bytes baixados da u-blox, injetando...", total);
    bool ok = gps_inject_agps(buf, (size_t)total);
    free(buf);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao injetar no modulo");
        return ESP_FAIL;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "ok - %d bytes de A-GPS injetados no modulo GPS", total);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Editor de pista (celular) - GET /editor serve um HTML autocontido
 * (sem CDN, sem mapa - usa a Geolocation API do proprio navegador) que
 * o usuario copia manualmente pra raiz do cartao como "editor.html".
 * Nao embutimos o HTML na flash pra nao inchar o binario do firmware.
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

static esp_err_t editor_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");

    FILE *f = fopen(WIFI_EDITOR_HTML_PATH, "r");
    if (!f) {
        httpd_resp_sendstr(req,
            "<html><body><h3>editor.html nao encontrado no cartao</h3>"
            "<p>Copie o arquivo track_editor.html para a raiz do cartao SD "
            "com o nome <code>editor.html</code>.</p></body></html>");
        return ESP_OK;
    }

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
        httpd_query_key_value(query, "name", name, sizeof(name));
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
        httpd_query_key_value(query, "name", name, sizeof(name));
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
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }

    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    size_t blen = strlen(base);
    if (base[0] == '\0' || strstr(base, "..") != NULL ||
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

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 20; /* default (8) nao sobra pra APIs + OTA + A-GPS + analise + evolucao */

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
    const httpd_uri_t evol_uri      = { .uri = "/evolucao",     .method = HTTP_GET,   .handler = evolution_get_handler };
    const httpd_uri_t api_sess_uri  = { .uri = "/api/sessions", .method = HTTP_GET,   .handler = api_sessions_handler };
    const httpd_uri_t api_laps_uri  = { .uri = "/api/laps",     .method = HTTP_GET,   .handler = api_laps_handler };
    httpd_register_uri_handler(s_httpd, &ota_uri);
    httpd_register_uri_handler(s_httpd, &agps_uri);
    httpd_register_uri_handler(s_httpd, &agps_dl_uri);
    httpd_register_uri_handler(s_httpd, &analysis_uri);
    httpd_register_uri_handler(s_httpd, &evol_uri);
    httpd_register_uri_handler(s_httpd, &api_sess_uri);
    httpd_register_uri_handler(s_httpd, &api_laps_uri);
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
     * causa nº1 do "scan nunca acha nada". */
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
