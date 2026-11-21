# F3 folytatás — a lokalizált blokker (Pro modellnek)

Ág: `slice-98-container-isolation` (a `main` zöld: `96c233b`). Ez a fájl a 98. szelet félkész állapotát és az
**egyetlen** nyitott hibát rögzíti, hogy ne kelljen újra végigpróbálni.

## Ami kész és mérve (ezen az ágon)

- Séma `0005`: `containers.proxy_mode/proxy_host/proxy_port/proxy_bypass/permissions`, validációval.
- Motor: partíciónként `CefRequestContext`, konténer-proxy (`system`/`direct`/`fixed`) a `OnRequestContextInitialized`
  hookban, engedély-politika (`CefPermissionHandler`, alapból megtagadva).
- `--partitiontest` (a `scripts/verify.ps1` közvetlenül a motorral futtatja): friss profilgyökér alatt **minden zöld** —
  külön kontextus, külön profilkönyvtár (nem üres), `proxy_one=fixed_servers|socks5://…`, `proxy_two=direct`.
- `cargo test` 147 zöld, `check-repo` ALL CHECKS PASSED.

## A blokker (reprodukálva, shell nélkül)

```
.\build\apps\engine-host\antikolt-engine-host.exe --partitiontest `
  --user-data-dir "C:/Users/info/AppData/Roaming/Antikolt Project/Antikolt/engine"
```

→ `EXIT=1`, és CEF ezt írja:

```
ERROR:cef\libcef\browser\chrome\chrome_browser_context.cc:116] Cannot create profile at path
ERROR:cef\libcef\browser\context.cc:160] The cache_path directory (…\engine\persist_…) is not a child of the
      root_cache_path directory (…)   ← korábbi, elválasztós próbálkozásnál
ERROR:cef\libcef\browser\context.cc:184] The cache_path is invalid. Defaulting to in-memory storage.
```

Ugyanez a teszt egy **friss** `--user-data-dir`-rel (a `--partitiontest` saját temp könyvtára) **zöld**.

**Következtetés (mérve, nem találgatás):** ebben a CEF-buildben (158.0.3, Chrome style) egy **további perzisztens
profil** létrehozása elutasításra kerül, ha a `CefSettings.cache_path`/`root_cache_path` gyökérben **már van profil**
(a shell motor-profilja `Default`-tal és `Local State`-tel). A profil létrehozása ilyenkor csendben memóriába esik, a
shell előnézet-munkása pedig a motor kilépését látja (`a motorház kilépett (kód 0)`), ezért a `previewtest` és a
`previewgentest` piros.

Kipróbálva és **nem** ez az ok: hiányzó szülőkönyvtár, üres maradék profilkönyvtár (ezt a kód most törli:
`empty_profile_removed=1`), `/` vs `\` elválasztó (mindkettőt mérve).

## Amit a Pro modellnek érdemes eldönteni

1. **Motor-példány konténerenként** (a shell már ma is több motor-folyamatot tud indítani): minden konténer saját
   `--user-data-dir`-t kap, így a globális profil maga a konténer profilja, és a `persist:` partíciók a fájlon belül
   maradnak. Ez a legkisebb natív rizikó, de több folyamat = több RAM.
2. **Globális gyökér áthelyezése**: a motor `CefSettings.cache_path`-ja kerüljön egy üres alkönyvtárba (pl.
   `engine/cef-root`), a `previews/snapshots/screenshots` maradjon a szülőben. Ha a CEF a *gyökérben* nem tűri a
   második profilt, de egy üres gyökérben igen, ez egy soros javítás — ezt kell először megmérni.
3. Ha egyik sem megy: a partíciók memóriában maradnak, és a bejelentkezés megőrzése a munkamenet-ledgeren keresztül
   történik (cookie-export/import) — ez viszont már termékdöntés.

## A folytatás lépései

1. A fenti 2. vagy 1. opció mérése a fenti egysoros paranccsal (a `--partitiontest` az ítélet: `profiles_on_disk=ok`).
2. Ha zöld: `scripts/verify.ps1` teljes kapu → a napló számait a `docs/HU/16-FEJLESZTESI-NAPLO.md` 98. szeletébe,
   majd merge a `main`-re.
3. A felületi szerkesztő (proxy + engedélyek a rácson) és a partíció-könyvtárak takarítása továbbra is nyitott.
