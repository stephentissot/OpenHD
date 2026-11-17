# Intégration OpenDroneID dans OpenHD

## 1. Statut et périmètre

- Date : 17 novembre 2026.
- Statut : spécification initiale, non implémentée ; validation matérielle en attente.
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
- L'authentification non interactive par clé a échoué (`publickey,password`).
- En conséquence, aucune commande de diagnostic n'a encore été exécutée sur le Pi.
- Le diagnostic est en attente d'authentification interactive dans le terminal.

À collecter après connexion, en lecture seule :

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