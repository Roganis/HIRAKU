# HIRAKU interface glossary (English, French, Japanese)

The words each translation keeps to, so that one thing has one name everywhere: the launcher, the
game menu, the boxes over a game and the performance overlay all read from one catalogue per
language. Add a line when a new term comes in; change one only with every place that uses it.

## Style

- **French**: vouvoiement; buttons and menu items in the infinitive ("Enregistrer", "Tout
  assigner"), instructions to the player in the imperative ("Appuyez sur…", "Tournez la
  platine…"); a no-break space before : ; ? ! and inside « »; "…" as one character.
- **Japanese**: です/ます in sentences, noun phrases for labels and buttons; full-width ：（）「」
  around Japanese text; a half-width space between Japanese and Latin words or numbers
  ("HIRAKU の", "%1 回").
- **Every language**: keep `%1`..`%9` (any order) and `%n` (a plural's number); keep `&` (the Alt
  letter: Japanese and Chinese put "(&F)" after the word) and HTML tags as they are; names of games,
  series, systems (DJ Main, System 573, Firebeat, Twinkle, Konami Python), programs (hiraku-pc),
  files, folders and options stay as written. Control names ("P1 Button 3") stay English: they are
  the bindings' keys. A game's own test-menu words (SOUND FLASH INIT, e-AMUSEMENT OPTIONS) stay as
  the game shows them.
- **Game-side texts** (contexts `menu`, `boxes`, `overlay`) are drawn in a box of fixed width over
  the game: keep them short. The overlay writes numbers with a "." in every language.

## Terms

| English | French | Japanese | note |
|---|---|---|---|
| binding (an input given to a control) | assignation | 割り当て | to bind: assigner / 割り当てる |
| Bind all | Tout assigner | すべて割り当て | |
| control (P1 Button 3, Turntable) | commande | 操作 | the names themselves stay English (decision 3) |
| Controls (the dialog, the button) | Commandes | 操作設定 | |
| controller / pad | manette | コントローラー / パッド | "Pad 0" = Manette 0 / パッド 0 |
| input (a key, a button pressed) | entrée | 入力 | |
| key (keyboard) | touche | キー | |
| key (IIDX, pop'n: white / black keys) | touche (blanche, noire) | 鍵盤 (白鍵, 黒鍵) | |
| button | bouton | ボタン | |
| turntable | platine | ターンテーブル | |
| axis / hat | axe / croix | 軸 / ハット | |
| layout (a controller's) | disposition | 配置 | |
| cabinet | borne | 筐体 | |
| board (the game's PCB) | carte | 基板 | |
| shelf (the cards' series) | étagère | 棚 | |
| game card | carte (de jeu) | カード | |
| mod | mod (m.) | MOD | |
| mods folder | dossier des mods | MOD フォルダー | |
| setting (one value) | réglage | 設定 / 項目 | a mod's or a shader's |
| Settings (the window) | Paramètres | 設定 | |
| save (the game's) | sauvegarde | セーブデータ | |
| save folder | dossier de sauvegarde | セーブフォルダー | |
| settings folder | dossier des paramètres | 設定フォルダー | |
| default | par défaut / valeur par défaut | 既定 / 既定値 | |
| Defaults (button) | Valeurs par défaut | 既定値 | |
| unlocks | déblocages | 解禁 | |
| event | événement | イベント | |
| code (unlock code) | code | コード | |
| DIP switches | commutateurs DIP | DIP スイッチ | |
| coinage / Free Play | monnayeur / Free Play | コイン設定 / フリープレイ | |
| operator | exploitant | オペレーター | |
| lights (the dialog) | éclairage | ランプ設定 | |
| lamp | lampe | ランプ | |
| LED | LED (inv.) | LED | |
| picture (the image settings) | image | 画面 | |
| shader | shader | シェーダー | |
| look (a shader preset) | rendu / préréglage | プリセット | |
| Off / Light / Full / Analog (picture modes) | Désactivé / Léger / Complet / Analogique | オフ / 軽量 / フル / アナログ | |
| test menu / I/O check | menu de test / test des entrées (I/O check) | テストメニュー / I/O チェック | |
| run (one start of a game, its log) | session | 実行 | |
| log | journal | ログ | |
| report (problem report) | rapport | レポート | |
| issue (GitHub) | ticket | issue | |
| launcher | lanceur | ランチャー | |
| update | mise à jour | アップデート | |
| release (a version) | version | リリース | |
| core (an emulated machine's program) | cœur | コア | |
| display sync | synchronisation à l'écran | 画面同期 | the overlay's panel |
| smooth frames (Settings > Display, display sync on) | images régulières | フレームを均等に表示 | not "fluide": that is smooth motion |
| smooth motion | mouvement fluide | なめらか表示 | |
| frame held for its refreshes | image tenue | フレームをそのまま表示 | |
| refresh (a display's) | rafraîchissement | リフレッシュ | |
| CRT beam | faisceau CRT | CRT ビーム | |
| performance overlay | affichage des performances | パフォーマンス表示 | |
| full screen / window | plein écran / fenêtre | フルスクリーン / ウィンドウ | |
| ready to play | prêt à jouer | プレイ可能 | |
| Player 1 | Joueur 1 | プレイヤー 1 | |
| on / off (a tick setting's value) | activé / désactivé | オン / オフ | |
| Publish lamps (a Settings option) | Publier les lampes | ランプ情報を公開 | Settings > Cabinet I/O: "Publier les lampes sur un socket local (…)" / "ランプ情報を公開（…）" |

| the game menu (hiraku-menu, from the cabinet) | le menu des jeux | ゲームメニュー | |
| Game menu (the menu bar's) | menu Jeu | 「ゲーム」メニュー | "(Game menu)" in a text |
| File / Game / Options / View / Help (menu bar) | &Fichier / &Jeu / &Options / &Affichage / Aid&e | ファイル(&F) / ゲーム(&G) / オプション(&O) / 表示(&V) / ヘルプ(&H) | menu paths in texts: "Jeu > Mods…", "ゲーム > MOD…" |
| Settings' pages: Display / Audio / Cabinet I/O / General / Look | Écran / Son / E/S de la borne / Général / Apparence | ディスプレイ / サウンド / 筐体 I/O / 全般 / 外観 | "Display" is not "Affichage" (the View menu) |
| Look (View > Look) | Apparence | 外観 | Pop, Pop night: names, not translated |
| Details / Log / Music (the docks) | Détails / Journal / Musique | 詳細 / ログ / 音楽 | |
| Play / Stop | Jouer / Arrêter | プレイ / 停止 | the music's Play: Lecture / 再生 |
| Refresh | Actualiser | 再読み込み | |
| Cards / List (views) | Cartes / Liste | カード / リスト | |
| first boot | premier démarrage | 初回起動 | |
| start (a game's) / starting a game | lancement / lancer | 起動 | |
| status (Ready / First boot) | état | 状態 | "Ready" in the list: Prêt / プレイ可能 |
| saves (the folder's contents) | sauvegardes | セーブデータ | |
| install cassette / install CD | cassette d'installation / CD d'installation | インストール用カセット / インストール CD | |
| network unit (e-AMUSEMENT) | unité réseau | ネットワークユニット | fitted: installée / 装着 |
| floppy disk / memory card | disquette / carte mémoire | フロッピーディスク / メモリーカード | |
| slot (memory card) | emplacement | スロット | |
| disk image / card image | image disque / image de carte | ディスクイメージ / カードイメージ | |
| dump (a game's files) | dump | ダンプ | "the dump MAME knows": le dump connu de MAME |
| ROM set | set de ROM | ROM セット | |
| recording (F6) | enregistrement | 録画 | exact = exact / 無劣化 |
| screenshot (F5) | capture d'écran | スクリーンショット | |
| encoder | encodeur | エンコーダー | |
| processor / CPU | processeur | CPU | |
| graphics card | carte graphique | グラフィックカード | |
| sound device / output | périphérique audio / sortie | サウンドデバイス / 出力 | |
| buffer | mémoire tampon / tampon | バッファー | |
| audio queue | file audio | オーディオキュー | |
| latency | latence | レイテンシ | |
| sound frames (48 kHz) | trames | フレーム | "%n frames (%1 ms)" |
| sound server | serveur de son | サウンドサーバー | |
| power plan (Windows) | mode de gestion de l'alimentation | 電源プラン | Windows' own names: Haute performance / 高パフォーマンス, Utilisation normale / バランス |
| press (a button press) judged | appui jugé | 判定 | judgement offset: décalage du jugement / 判定オフセット |
| synth | synthé | シンセ | SoundFont stays |
| song (the launcher's music) | morceau | 曲 | a song in a game: chanson |
| Shuffle / Autoplay | Aléatoire / Auto | シャッフル / 自動再生 | |
| server inside HIRAKU | serveur intégré à HIRAKU | HIRAKU 内蔵のサーバー | |
| card reader / 10-key | lecteur de carte / pavé numérique | カードリーダー / テンキー | |
| I/O board | carte d'E/S | I/O ボード | |
| spice2x option names (EA Service URL, PCBID, Player 1 Card, Auto Card Insert) | kept in English | kept in English | they match spicecfg's |
| portable install | installation portable | ポータブル版 | |
| fast start-up | démarrage rapide | 高速起動 | |
| title screen | écran titre | タイトル画面 | |
| market (a cabinet's) | marché | 出荷地域 | |
| Language / Follow the system | Langue / Celle du système (%1) | 言語 / システムに合わせる（%1） | languages listed by their own names |
| Restart now / Later | Redémarrer maintenant / Plus tard | 今すぐ再起動 / 後で | |
| status Ready / Not built / Not installed / Not in this package | Prêt / Non compilé / Non installé / Absent de ce paquet | プレイ可能 / 未ビルド / 未インストール / このパッケージにはありません | the game list's status column |
| binaries folder (Settings' "Game executables") | dossier des exécutables | 実行ファイルのフォルダー | |
| package (a release's) | paquet | パッケージ | "Linux packages": paquets Linux / Linux 版パッケージ |
| About / Software / Licences / References (About's tabs, headings) | À propos / Logiciels / Licences / Références | 概要 / ソフトウェア / ライセンス / 参考 | |
| contributors | contributeurs | 貢献者 | |
| the boxes over a game (context `boxes`) | — | — | short: drawn in a fixed-width box; a line breaks by itself |
| kept (a picture, a recording) | gardé(e) | 保存しました | "Picture kept": Image gardée / 画像を保存しました |
| taken out / in the drive / in its slot | retiré(e) / dans le lecteur / dans son emplacement | 取り出しました / 挿入しました | |
| GAME MENU (heading) | MENU DES JEUX | ゲームメニュー | |
| FAST START-UP (heading) | DÉMARRAGE RAPIDE | 高速起動 | |
| sound device stopped | le périphérique audio s'est arrêté | サウンドデバイスが停止しました | |
| performance overlay (context `overlay`) | — | — | technical and short: "." for decimals in every language |
| fps / ms / Hz / MHz / % | i/s / ms / Hz / MHz / % | fps / ms / Hz / MHz / % | |
| late (a frame) / break (in the sound) | en retard, retards / coupure | 遅れ / 途切れ | |
| display sync | synchro écran | 画面同期 | the overlay's short form of "synchronisation à l'écran" |
| work (a frame's) / window (its drawing) | travail / fenêtre | 処理 / ウィンドウ | |
| thread | thread | スレッド | |
