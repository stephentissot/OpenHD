# Intégration OpenDroneID dans OpenHD

## 1. Statut et périmètre

- Date : 17 novembre 2026.
- Statut : spécification initiale, non implémentée ; diagnostics Ground et Air
  réalisés, validation des données MAVLink réelles et émissions radio en attente.
- Base analysée : branche `2.7-evo`, commit `c75c37e6bcd014a990b81ae9b2556eb6afefd924`.
- Cible initiale : unité Air Raspberry Pi Zero 2 W, caméra IMX708,
  adaptateur BL-M8812EU2 pour OpenHD, contrôleur STM32H723 sous ArduPilot.
- Objectif : diffuser les données Remote ID via les radios intégrées du Pi,
  sans ESP32 et sans modifier la radio USB dédiée à la vidéo.
- L'UART1 désigne ici le périphérique du STM32 ; son équivalent port logique
  ArduPilot et le périphérique Linux doivent être vérifiés séparément.
- Une réception correcte ne démontre pas à elle seule une conformité
  réglementaire. Remote ID européen et signalement électronique français
  doivent être évalués séparément selon l'usage.

## 2. Éléments vérifiés dans les sources

### Configuration

- [Configuration système](../OpenHD/ohd_common/src/openhd_config.cpp) :
  `load_config()` obtient les paramètres de SysUtils puis conserve une copie
  statique. Ajouter un champ à `Config` seul ne crée pas une configuration
  complète ni un mécanisme de mise à jour à chaud.
- [Persistance](../OpenHD/ohd_common/inc/openhd_settings_persistent.h) :
  `PersistentSettings<T>` fournit chargement, valeurs par défaut, écriture et
  notification ; ne fournit pas de verrouillage entre threads.
- [Paramètres](../OpenHD/ohd_common/inc/openhd_settings_imp.h) :
  `Setting`, `IntSetting` et `StringSetting` portent les callbacks de validation.
- [Exemple Air](../OpenHD/ohd_telemetry/src/AirTelemetrySettings.h) et
  [exposition MAVLink](../OpenHD/ohd_telemetry/src/AirTelemetry.cpp) : les
  paramètres des modules sont agrégés avant `settings_generic_ready()` dans
  [main](../OpenHD/main.cpp). Les identifiants MAVLink sont limités à 16 caractères.
- Le répertoire persistant actuel est `/usr/local/share/openhd/`.

### Télémétrie et threads

- `AirTelemetry::setup_uart()` configure le lecteur série FC et son callback
  `on_messages_fc()`.
- `on_messages_fc()` enregistre puis retransmet les messages vers le Ground ;
  c'est le point d'observation proposé pour Remote ID.
- Les composants `MavlinkComponent` ne reçoivent pas automatiquement le flux FC :
  leur ajout à `m_components` ne suffit donc pas.
- [OHDTelemetry](../OpenHD/ohd_telemetry/src/OHDTelemetry.cpp) possède déjà un
  thread de boucle ; les endpoints série possèdent également leur thread.
- Les headers MAVLink locaux contiennent les messages `OPEN_DRONE_ID_*`.

### Gestion radio

- [Découverte Wi-Fi](../OpenHD/ohd_interface/src/wifi_card_discovery.cpp) :
  distinction Broadcom/Realtek et sélection des rôles vidéo/hotspot.
- [OHDInterface](../OpenHD/ohd_interface/src/ohd_interface.cpp) interdit déjà
  d'utiliser une interface vidéo comme hotspot/client.
- [Hotspot](../OpenHD/ohd_interface/src/wifi_hotspot.cpp) : la voie C++ actuelle
  utilise NetworkManager et `nmcli`, connexion `ohd_wifi_hotspot`, adresse
  `192.168.3.1/24`. Le script Buildroot présent dans le dépôt utilise une autre
  approche ; sa présence ne prouve pas son utilisation sur le Pi.
- Aucun backend Bluetooth Remote ID n'a été identifié dans le cœur inspecté.
- Le matériel Zero 2 W est spécifié Bluetooth 4.2 : BLE Legacy envisageable,
  mais pas BLE 5 Coded PHY / Long Range avec son contrôleur intégré.

## 3. Contrat ArduPilot local

L'analyse du dépôt compagnon ArduPilot de l'installation a identifié une
extension locale de `AP_OpenDroneID` ; elle n'est pas une API OpenHD upstream.
Le comportement du firmware flashé reste à confirmer par observation.

| Élément | Comportement des sources locales |
| --- | --- |
| Activation | `DID_ENABLE` |
| Port | `DID_MAVPORT`, port logique ArduPilot, pas numéro brut UART |
| Transports | `DID_TX_BLE`, `DID_TX_NAN`, `DID_TX_BCN` |
| Commande | `COMMAND_LONG`, commande `31000` (`MAV_CMD_USER_1`) |
| Signature | `param1 = 4944` |
| Masque | `param2` entier : bit 0 BLE, bit 1 NAN, bit 2 Beacon |
| Cible | système et composant à zéro (broadcast) |
| Rafraîchissement commande | au changement et toutes les 5 secondes |
| Données dynamiques | Location et System Update prévues à 1 Hz |
| Données statiques | Basic ID, System, Self ID, Operator ID répartis sur 3 s |
| Retour transpondeur | `OPEN_DRONE_ID_ARM_STATUS` sur la liaison FC |

Exiger MAVLink 2. Décoder les messages déjà parsés, sans sérialiser puis
reparser les octets et sans créer de second lecteur UART.

La désactivation `DID_ENABLE=0` arrête les mises à jour côté FC ; elle n'envoie
pas explicitement un masque nul. Ne pas interpréter le dernier masque comme
une autorisation perpétuelle.

L'adaptateur custom doit être optionnel, vérifier source FC, commande,
signature, cibles compatibles et valeur finie/entière du masque dans [0,7].
Il ne doit pas intercepter les autres usages de `MAV_CMD_USER_1` ni répondre
automatiquement au nom d'un autre composant. La politique ACK est à convenir
avec le contrat custom avant implémentation.

## 4. Architecture proposée

Créer un module Air dédié, compilable optionnellement, fondé sur la bibliothèque
officielle `opendroneid-core-c`, avec worker interne au processus OpenHD.
Les noms ci-dessous désignent des interfaces proposées, pas des classes existantes.

1. `AirTelemetry` remet les messages sélectionnés à `OpenDroneIdManager`.
2. L'adaptateur MAVLink valide et copie les champs dans un état borné.
3. Le gestionnaire conserve séparément données, configuration et commandes FC.
4. Le worker prend un instantané cohérent puis encode et diffuse hors verrou.
5. Un arbitre radio coordonné avec `OHDInterface` autorise ou refuse les backends.
6. Le statut réel revient à la télémétrie ; le retour FC est envoyé via l'endpoint
   existant et non via un nouveau port série.

Le chemin existant vers le Ground reste intact. L'absence de Ground ou de liaison
vidéo ne doit pas empêcher la diffusion si les données FC sont disponibles.

### Concurrence et cycle de vie

- Callback UART court : ni commande système, ni accès HCI, ni écriture disque,
  ni attente d'un worker radio.
- État « dernière valeur » par message ou file bornée avec politique de surcharge
  explicite ; ne pas perdre une configuration/statique derrière un flot dynamique.
- Protection par mutex de courte durée ; aucune référence à une structure mutable
  de `PersistentSettings` ne doit échapper au thread propriétaire.
- Worker réveillé par condition variable, horloge monotone pour les échéances.
- Les timestamps transmis restent ceux de leur référence UTC/époque protocolaire ;
  ne jamais remplacer l'heure Remote ID par l'uptime du Pi.
- Gestion explicite des erreurs et des timeouts des backends ; une exception
  interceptable ne doit pas arrêter OpenHD. Un thread ne protège toutefois pas
  d'un crash mémoire du processus entier.
- Arrêt : fermer l'entrée/désinscrire les callbacks, attendre les callbacks en
  cours, réveiller le worker, désactiver les annonces, joindre le worker, détruire.
- Créer les dépendances avant l'activation du callback UART ; traiter aussi l'ordre
  de destruction des endpoints et du module, pas uniquement la boucle principale.

## 5. Configuration proposée

Utiliser un holder dédié avec fichier JSON distinct sous le répertoire persistant
de télémétrie. Ne pas modifier le schéma UART pour cette première intégration.
Valeurs manquantes : valeurs par défaut ; valeurs invalides : erreur explicite et
activation interdite. Préserver les champs reconnus et tester les migrations.

| Paramètre | Type / défaut proposé | Rôle |
| --- | --- | --- |
| `ODID_ENABLE` | bool interne `openDroneId`, exposé 0/1 ; 0 | autorisation globale locale |
| `ODID_CTRL_SRC` | entier ; LOCAL | LOCAL ou FC_CUSTOM, sans mélange implicite |
| `ODID_BLE` | 0/1 ; 0 | demande BLE en mode LOCAL |
| `ODID_BEACON` | 0/1 ; 0 | demande Beacon en mode LOCAL |
| `ODID_WIFI_IF` | chaîne ; vide | sélection validée de la radio intégrée |
| `ODID_BT_DEV` | chaîne ; vide | contrôleur HCI sélectionné et identifié |

En mode FC_CUSTOM, les paramètres de transport locaux ne remplacent pas le masque
FC. L'autorisation globale locale reste nécessaire. Les commandes FC ne sont pas
persistées à chaque réception. Refuser les chaînes d'interface arbitraires dans
des commandes shell ; utiliser des API/arguments sûrs.

NAN est explicitement non pris en charge en phase initiale ; une demande NAN doit
être visible comme non satisfaite et non être convertie silencieusement en Beacon.
Exposer séparément demande, capacité et état effectif, sans afficher une réussite
parce que le paramètre d'activation a été accepté.

L'acceptation d'un paramètre dynamique doit notifier le worker ; elle ne doit pas
reconfigurer le Wi-Fi depuis le callback du serveur de paramètres. Tout changement
de politique en vol doit faire l'objet d'une décision explicite avant déploiement.

## 6. Validité des données et statut FC

- Accepter uniquement le drone configuré et les cibles prévues ; distinguer les
  données locales des observations d'autres aéronefs (`id_or_mac`).
- Prendre en charge Basic ID, Location, System, System Update, Self ID et Operator
  ID ; décider et tester séparément Auth et Message Pack avant de les annoncer.
- System Update actualise uniquement les champs concernés, sans effacer les
  autres données System.
- Conserver plusieurs Basic ID selon les limites de la bibliothèque retenue.
- Suivre séparément la fraîcheur monotone de Location, System/Update, commande
  et source FC ; invalider les caches appropriés lors d'un redémarrage FC détecté.
- Proposition de seuils de banc, non normative : Location périmée après 3 s,
  commande custom périmée après 15 s. Ces seuils doivent être revus selon la
  spécification applicable, les pertes observées et la politique de panne.
- Ne jamais rendre fraîche une position périmée en réécrivant son timestamp.
- En panne : état dégradé explicite ; contenu radio et poursuite/arrêt des annonces
  à définir selon le profil réglementaire, sans prétendre une position valide.
- Émettre périodiquement `OPEN_DRONE_ID_ARM_STATUS` via le chemin FC existant,
  cadence de banc proposée 1 Hz. Choisir les identités MAVLink avec les mainteneurs
  et vérifier le routage pour que ce message ne soit pas filtré comme local.
- `GOOD_TO_ARM` uniquement si les préconditions réellement exigées sont remplies ;
  un appel API réussi n'est pas une preuve de réception radio ni de conformité.
- Ne pas contourner les contrôles d'armement ArduPilot. La disponibilité d'une
  position opérateur fournie par la station doit être vérifiée séparément.

## 7. Radios et préservation de SSH

### Règles impératives

- Aucun accès OpenDroneID aux interfaces/PHY wifibroadcast USB.
- Identifier la radio intégrée par son matériel/pilote/chemin sysfs et non par
  l'hypothèse `wlan0`. Contrôler aussi les PHY partagés par interfaces virtuelles.
- Ne pas arrêter NetworkManager, changer le canal, passer en monitor, réinitialiser
  HCI, décharger un pilote ou modifier un overlay pendant le diagnostic.
- Aucun second AP indépendant sur le PHY du hotspot.
- Wi-Fi/Bluetooth intégrés partagent des contraintes RF : tester la coexistence,
  même si les services Linux semblent indépendants.

### Backends

1. DRY_RUN : validation et encodage, aucune émission.
2. BLE_LEGACY : vérifier HCI/firmware/BlueZ, puis choisir une API appropriée à
   l'actualisation des annonces et à la cadence. Ne pas supposer qu'une succession
   d'enregistrements D-Bus satisfait les contraintes de timing.
3. WIFI_BEACON : prototype Message Packs complets sur le pilote installé ;
   coordonner avec le propriétaire AP existant. NetworkManager ne garantit pas
   l'existence d'une API utilisable pour ces éléments vendor dynamiques.

Le partage Beacon + hotspot est une hypothèse à valider, pas une capacité acquise.
Si impossible avec la pile installée, déclarer le backend indisponible et conserver
SSH ; ne pas prendre automatiquement le contrôle de la radio.

Vérifier également si un overlay désactive Bluetooth pour libérer un UART du Pi.
Ne pas confondre cet UART avec UART1 côté STM32. Une réactivation pourrait casser
la télémétrie existante ; elle exige une analyse préalable des broches et aliases.

## 8. Diagnostic matériel : état au 17 novembre 2026

Accès demandé : `192.168.3.1`, compte `openhd`. Aucun secret n'est stocké ici.

- Le serveur SSH répond à cette adresse.
- Sa clé diffère de l'entrée locale connue. L'utilisateur a confirmé l'identité
  présentée pour cette session ; le fichier global des hôtes connus n'a pas été modifié.
- L'authentification non interactive par clé a échoué (`publickey,password`),
  puis les lectures ont réussi après authentification interactive.
- Aucune installation, reconfiguration, émission de test ni interruption de
  service n'a été effectuée.

### Résultats effectivement relevés

| Élément | Résultat |
| --- | --- |
| Rôle | **Ground**, marqueur `/boot/openhd/ground.txt` présent ; SSID `openhd_ground` |
| Matériel | Raspberry Pi Zero 2 W Rev 1.0 |
| OS | Raspbian GNU/Linux 11 Bullseye, userland 32 bits |
| Noyau | `6.1.29-v7+`, `armv7l` |
| OpenHD installé | `2.6.4-202505221612-d9ed491` |
| Service OpenHD | actif, `/usr/local/bin/openhd`, exécuté en root |
| Radio USB vidéo | `wlan0`, `phy0`, pilote `rtl88x2eu_ohd`, chemin matériel USB |
| Radio intégrée | `wlan1`, `phy1`, pilote `brcmfmac`, chemin matériel SDIO |
| Hotspot | NetworkManager, `ohd_wifi_hotspot`, `wlan1`, `192.168.3.1/24` |
| Fréquences observées | hotspot et radio monitor sur canal 1, 2412 MHz |
| Modes intégrés annoncés | managed/AP/P2P/IBSS ; pas de monitor ni NAN annoncés |
| Combinaison AP annoncée | un seul AP, combinaison correspondante limitée à un canal |
| Bluetooth | `hci0`, bus UART, Broadcom, HCI/LMP 4.2, UP RUNNING |
| Capacités BLE annoncées | LE et Advertising ; rôle peripheral disponible |
| Annonces BlueZ | 0 instance active, 5 instances supportées annoncées par BlueZ |
| rfkill | Wi-Fi et Bluetooth non bloqués, matériel et logiciel |
| BlueZ | `5.55-3.1+rpt2`, service actif |
| NetworkManager | `1.30.6-1+deb11u1`, service actif |
| UART visible | `/dev/serial0` pointe vers `/dev/ttyS0` |
| Boot UART | `enable_uart=1`, lignes overlays uart2/uart3 présentes ; leur effet n'est pas établi |
| RAM au relevé | 426 Mio total, environ 152 Mio disponibles ; swap 45/99 Mio utilisé |
| Disque racine | 677 Mio libres sur 3,7 Gio, occupation 82 % |
| Outils de build | GCC, G++, CMake et Git non trouvés aux emplacements usuels vérifiés |
| Outils radio | `iw` et `rfkill` présents sous `/usr/sbin`, absent du PATH SSH observé |
| Heure système | mai 2023, incohérente avec la date de session ; ne pas utiliser cette heure comme référence Remote ID |

Ces résultats concernent le **Pi Ground connecté**, pas encore le Pi Air du drone.
Le rôle n'a pas été changé. Les données UART/Remote ID effectivement émises par le
STM32H723 n'ont pas été observées. Les journaux du service ne sont pas exploitables
avec les permissions et la sortie console constatées dans ce diagnostic.

### Conséquences pour le développement

- La présence HCI/BlueZ permet d'envisager un prototype BLE Legacy sur ce Pi,
  sans preuve à ce stade de réception ni de cadence correcte.
- Le nombre d'instances annoncé ne démontre ni annonces étendues, ni Long Range.
- L'absence de monitor sur le Broadcom interdit de compter sur cette voie sans
  modification de pile ; l'émission Beacon via AP reste une question différente.
- Le hotspot et la radio vidéo partagent actuellement la bande et le canal :
  mesurer la coexistence, sans changer ce réglage automatiquement.
- Ne pas déployer directement le binaire du fork `2.7-evo` à la place du 2.6.4 :
  relever d'abord la révision Air et définir une base logicielle/image compatible,
  notamment les dépendances SysUtils de la branche récente.
- Privilégier un build ARM 32 bits hors cible avec sysroot compatible, ou une image
  de développement dédiée ; l'installation d'une toolchain complète sur cette
  racine et un build lourd sont risqués vu le disque et la mémoire disponibles.
- Refaire l'inventaire sur l'Air avant de figer les noms d'interface et l'UART.
- Préserver les timestamps du FC et définir la stratégie horaire ; ne pas régler
  automatiquement l'horloge pendant le diagnostic.

### Diagnostic complémentaire de l'unité Air

Après changement de hotspot par l'utilisateur, une nouvelle lecture SSH sur
`192.168.3.1` confirme le marqueur `/boot/openhd/air.txt` et le SSID `openhd_air`.
Le matériel, les versions OS/noyau/OpenHD/BlueZ/NetworkManager sont identiques
aux versions Ground relevées ci-dessus. Les services OpenHD, Bluetooth et
NetworkManager sont actifs. Aucune configuration ni émission n'a été modifiée.

| Élément Air | Résultat |
| --- | --- |
| Wi-Fi intégré | **`wlan0`, `phy0`**, SDIO, pilote `brcmfmac`, AP hotspot |
| Wi-Fi USB vidéo | **`wlan1`, `phy1`**, USB, pilote `rtl88x2eu_ohd`, monitor |
| Hotspot | `ohd_wifi_hotspot`, `192.168.3.1/24`, canal 1 / 2412 MHz |
| Vidéo radio | canal 1 / 2412 MHz également au relevé |
| Capacités Wi-Fi intégré | AP/managed/P2P/IBSS ; ni monitor ni NAN annoncés ; un AP maximum, combinaison AP sur un canal |
| Bluetooth | `hci0`, Broadcom UART, HCI/LMP 4.2, UP RUNNING, non bloqué |
| Publicité BLE | LE/Advertising annoncés, 0 instance active et 5 supportées annoncées par BlueZ |
| UART visible | `/dev/serial0` vers `/dev/ttyS0` |
| Configuration FC persistée | `fc_uart_connection_type=DEFAULT`, 115200 bauds, flow control désactivé |
| Caméra | overlay `imx708` présent ; pas de test caméra réalisé |
| RAM au relevé | 426 Mio total, 259 Mio disponibles, swap non utilisé |
| Disque racine | 712 Mio libres, occupation 81 % |
| Build | GCC/G++/CMake/Git absents des chemins usuels vérifiés |
| Heure système | mai 2023, toujours impropre comme référence UTC actuelle |

**Les noms d'interfaces sont inversés entre Air et Ground** : la protection des
radios doit impérativement utiliser l'identité matérielle, le pilote et le PHY.
La configuration UART persistée ne constitue pas une preuve d'ouverture du port
ni de réception des messages ODID ; aucun deuxième lecteur série n'a été ouvert.
Le Bluetooth est déjà actif, donc aucune réactivation d'overlay n'est requise
pour démarrer l'étude BLE sur cette image. La réception et la cadence effective
des annonces restent à tester.

Le décalage 2.6.4 installé / 2.7-evo développé est confirmé sur l'Air également.
Choisir la stratégie de version/image avant tout remplacement de binaire.
Les constats antérieurs signalés « Air en attente » dans le compte rendu Ground
doivent se lire historiquement ; le présent complément actualise l'inventaire.

Inventaire de référence (données MAVLink réelles encore à compléter) :

| Domaine | Preuves attendues |
| --- | --- |
| Système | distribution, noyau, architecture userland, modèle, version OpenHD |
| Wi-Fi | interfaces, PHY, pilotes, modes et combinaisons supportés |
| Hotspot | interface réelle, gestionnaire/service, connexion active, fréquence |
| Bluetooth | contrôleurs sysfs/HCI, rfkill, version BlueZ, état services |
| UART | liens serial, paramètres FC non sensibles, overlays UART/BT |
| Développement | compilateur, CMake, Git, espace disque, mémoire |

Ne pas publier de mot de passe Wi-Fi, clé privée, identifiant opérateur ou
coordonnée GNSS dans les comptes rendus. L'inventaire des noms et versions suffit.

## 9. Plan de développement et critères de validation

| Étape | Critère de sortie |
| --- | --- |
| D0 diagnostic | radios, UART et version image identifiés sans perte SSH |
| D1 build de référence | même révision compilée dans un environnement Linux compatible avec l'image |
| D2 cœur DRY_RUN | tests encodage, unités, timestamps, messages partiels/périmés et migrations JSON |
| D3 partage MAVLink | un seul lecteur série, statistiques ODID, retransmission OSD conservée |
| D4 contrôle custom | masques 0..7, sources invalides, NaN/inf, timeout, redémarrage FC testés |
| D5 BLE | réception indépendante et vérification des cadences, vidéo/SSH conservés |
| D6 Beacon | packs complets décodés, actualisation démontrée, hotspot conservé |
| D7 intégration | statut FC honnête, démarrage/arrêt et charge prolongée validés |

Ajouter des tests de concurrence, arrêt avec callback actif, surcharge de données,
backend absent/occupé et changement de configuration répété. Tester que
`ODID_ENABLE=0` ne produit aucune annonce ni prise de contrôle radio.

Sur banc : hélices retirées, émissions de test en environnement RF maîtrisé.
Mesurer CPU/RAM/température/sous-tension, latence callback et régularité des
annonces ; comparer vidéo et télémétrie à une référence sans Remote ID.

Déploiement manuel après validation ; aucune programmation du FC ni remplacement
automatique du binaire OpenHD. Sauvegarder la carte SD et prévoir le retour arrière.

## 10. Contribution upstream et décisions ouvertes

- Proposer l'architecture aux mainteneurs avant une grosse PR.
- Choisir version/licence des dépendances après examen des fichiers réels et de
  leurs notices ; ne pas se limiter aux badges des dépôts.
- Éviter d'embarquer intégralement `transmitter-linux` et ses versions historiques
  de BlueZ/hostapd. Le considérer comme une référence de prototypage.
- Backend custom ArduPilot isolé et désactivable ; cœur compatible avec les
  messages MAVLink standards sans supposer la présence de ce firmware custom.
- Dépôts complémentaires potentiels : SysUtils/WebUI, QOpenHD, image builder.
- Décisions bloquantes : API Bluetooth, accès aux éléments Beacon avec le hotspot,
  politique de panne radio, identité MAVLink et règles d'activation en vol.

Références : [Core C](https://github.com/opendroneid/opendroneid-core-c),
[émetteur Linux](https://github.com/opendroneid/transmitter-linux),
[service MAVLink](https://mavlink.io/en/services/opendroneid.html),
[spécifications Zero 2 W](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/).