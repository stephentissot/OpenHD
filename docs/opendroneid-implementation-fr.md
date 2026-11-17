# OpenDroneID Air — état de l'implémentation

## Périmètre livré

Intégration Air avec BLE BlueZ **expérimental, hors matériel** ; ni émission RF
validée ni certification.
Le codec officiel Apache-2.0 est un sous-module épinglé à
`849c03adbe0cdd68997a462fafab397edcfe53f3` (voir
[notice de dépendance](../third_party/OPENDRONEID.md)). Le dépôt OpenHD est
autonome après initialisation de ses sous-modules.

Les options CMake `OPENHD_ENABLE_ODID` et `OPENHD_ODID_BLUEZ` valent **OFF**
par défaut (voir [intégration CMake](../OpenHD/ohd_telemetry/CMakeLists.txt) et
[module ODID](../OpenHD/ohd_telemetry/src/opendroneid/CMakeLists.txt)).
Quand `OPENHD_ENABLE_ODID` est OFF,
aucun nouveau code, paramètre, worker ou accès aux fichiers ODID n'est inclus.
Quand elle est ON, le runtime reste désactivé (`ODID_ENABLE=0`, `ODID_DRY_RUN=1`).
Les deux options ON câblent le backend BLE au worker de `Manager` ; cela
n'autorise pas son émission à soi seul. Le worker désactivé dort
alors sur une condition variable ; aucune annonce, commande FC, écriture de
configuration initiale ou prise de contrôle radio n'est effectuée par ODID.

## Flux et validité

- Un seul endpoint UART FC : observation des messages déjà décodés dans
  `AirTelemetry::on_messages_fc()`. Le passage Ground/OSD existant est conservé.
  Ni nouveau parseur, ni ouverture UART, ni requête de débit au FC.
- Filtrage MAVLink 2, `FC_SYS_ID` explicite (1–254), composant FC configurable
  (`ODID_FC_COMP`, défaut `MAV_COMP_ID_AUTOPILOT1=1`). Cibles acceptées :
  système broadcast ou système FC, composant broadcast ou
  `MAV_COMP_ID_ODID_TXRX_1=236`, vérifié dans le dialecte du dépôt. Les messages
  standards avec `id_or_mac` non nul sont des observations étrangères : rejet.
- Dernières valeurs bornées : deux BasicID (un slot par type, mise à jour en
  place, pas d'éviction silencieuse), Location, System, SelfID, OperatorID.
  SystemUpdate nécessite un System complet préalable et ne remplace que
  coordonnées/altitude opérateur et timestamp. Auth/MessagePack non pris en charge.
- Conversion cdeg/cm/s/degE7 vers unités du codec. Validation des enums,
  coordonnées, sentinelles, NaN/Inf et plages **avant** encodage C. Les timestamps
  FC sont préservés (quantification normale du codec), jamais l'heure Linux.
- Location et System/Update expirent à 3 s, source à 5 s, commande à 15 s,
  mesurées en horloge monotone. Répéter le même timestamp dynamique ne renouvelle
  pas sa fraîcheur. Location accepte le passage d'heure, rejette les retours
  temporels (demi-plage 1800 s) ; System/Update rejette un timestamp décroissant.
- Un recul de `SYSTEM_TIME.time_boot_ms` >2 s invalide toutes les données et le
  masque (hors wrap uint32). Un trou source ≥5 s fait de même au prochain message.
  **Limite : sans SYSTEM_TIME, un reboot rapide sans trou de 5 s n'est pas
  prouvablement détectable.** Aucun abonnement supplémentaire n'est imposé au FC.
- Politique de panne de banc : pas de frames produites lorsque le jeu complet
  exigé est absent/périmé. Cela n'est pas une politique réglementaire validée.
  SelfID/OperatorID et position opérateur sont exigés par ce profil conservateur.

## Configuration et contrôle

Fichier JSON distinct dans le répertoire de télémétrie, nom
`opendroneid.json`, schéma 1. Champs manquants : défauts sûrs. Types/valeurs/schema
invalides : activation bloquée, fichier conservé, diagnostic explicite ; réparer
le fichier puis redémarrer. Les champs reconnus sont conservés lors d'une
migration partielle ; les champs inconnus ne sont pas conservés à la réécriture.
Écriture temporaire puis rename ; pas de garantie fsync contre coupure secteur.
Les callbacks de paramètres sérialisent l'accès au holder dédié ; aucune
référence mutable n'est partagée avec le worker. Pas d'I/O fichier côté FC.

| Paramètre | Défaut | Sens |
|---|---:|---|
| `ODID_ENABLE` | 0 | autorisation locale globale |
| `ODID_CTRL_SRC` | 0 | LOCAL=0, FC_CUSTOM=1 |
| `ODID_BLE`, `ODID_NAN`, `ODID_BEACON` | 0 | demandes LOCAL, pas capacités |
| `ODID_DRY_RUN` | 1 | validation/encodage sans émission |
| `ODID_FC_COMP` | 1 | composant source FC |
| `ODID_BT_DEV`, `ODID_WIFI_IF` | vide | HCI BLE explicite ; Wi-Fi non implémenté, aucun shell |
| `ODID_CAP`, `ODID_EFFECTIVE` | 0 | bit BLE=1 seulement après inscription BlueZ locale courante réussie ; pas une réception RF, lecture seule |
| `ODID_REQUEST` | 0 | masque demandé vivant, lecture seule |
| `ODID_STATE` | disabled | état/erreur, lecture seule |

Tous les identifiants font au plus 16 caractères. Modifier la politique exige
d'abord de désactiver ODID, puis configurer, puis réactiver explicitement.
Chaque changement/source invalide caches et autorisation. La modification du
`FC_SYS_ID` existant reste possible et invalide immédiatement l'état ODID.
Il n'y a pas de verrouillage selon armement FC : ne pas considérer ceci comme
une autorisation de changer la politique en vol.

FC_CUSTOM est exclusivement l'extension locale documentée : `COMMAND_LONG`,
31000, `param1=4944`, `param2` fini entier 0..7, source et cibles filtrées.
Les paramètres transports LOCAL n'écrasent jamais le masque custom. Expiration
15 s, aucun ACK produit, aucune commande consommée/supprimée du flux Ground.
Les commandes ne sont jamais persistées.

## Worker, arrêt et retour FC

Le callback FC ne fait que décoder/valider/copier sous mutex court : aucune I/O
radio dans le callback UART. Le [worker de Manager](../OpenHD/ohd_telemetry/src/opendroneid/Core.cpp)
prend une copie puis encode et appelle le backend hors verrou. Il attend 200 ms
après chaque itération, avec réveil sur politique ou arrêt. La rotation BLE
avance d'une trame par inscription réussie : **200 ms plus les opérations**,
dont jusqu'à 900 ms de budget d'attente inscription/mise à jour, les lectures
sysfs et les désinscriptions. Ni période RF ni cadence réglementaire garantie.
Une péremption demande le retrait à l'itération suivante (200 ms plus les
opérations backend à attente bornée déjà engagées). Après I/O, données courantes,
arrêt et génération sont revérifiés avant publication/retrait ; cela ne fixe
pas une échéance RF stricte. Pas de file non bornée ; un résultat de génération
obsolète n'est pas publié.

Les callbacks endpoints capturent une barrière de vie partagée, fermée avant
destruction : plus d'entrée, attente des callbacks actifs sans verrou endpoint,
arrêt/join des producteurs UART, réveil/join du worker, destruction des membres.
Ce join concerne le worker `Manager`, pas le thread privé détaché BlueZ : son
contexte reste vivant pendant le nettoyage et la fermeture asynchrone.
`BluezBackend::stop()` peut revenir avant un retrait RF prouvé ; ni join complet
de tous les threads ni absence absolue de fuite ne sont démontrés.
Les callbacks de paramètres capturent une référence faible au module.
Les threads du transport OpenHD historique ne sont pas réécrits : les tests
du cœur et de la barrière ne prouvent pas l'absence de races de tout OpenHD.

À 1 Hz quand autorisé, la boucle télémétrie envoie ARM_STATUS via **l'endpoint FC
existant directement**, sans filtre des messages locaux OpenHD. Identité :
système FC configuré, composant ODID 236 ; jamais identité autopilote usurpée.
Le statut est **toujours PRE_ARM_FAIL_GENERIC**, y compris avec données complètes
et inscription BlueZ réussie. Aucun GOOD_TO_ARM tant que la RF n'est pas validée,
ni succès radio fictif. La transmission utilise
le chemin série existant, dont les écritures peuvent bloquer : elle n'est pas
une garantie de temps réel ni une preuve de réception par ArduPilot.

## Backend BLE expérimental ; NAN/Beacon absents

Avec les deux options CMake ON, `ODID_ENABLE=1`, `ODID_DRY_RUN=0`, une demande
BLE et des données complètes fraîches, le worker peut inscrire une annonce via
D-Bus BlueZ. Sans `OPENHD_ODID_BLUEZ`, aucun backend RF n'est compilé.
`ODID_CAP=1` indique uniquement une inscription locale BlueZ courante réussie ;
`ODID_EFFECTIVE=1` a la même portée locale, jamais une preuve de réception RF.
NAN et Beacon restent absents et leurs bits non satisfaits.

`ODID_BT_DEV` doit sélectionner explicitement un `hciN`. La vérification
fail-closed exige carte Raspberry Pi et preuves sysfs/Device Tree d'un contrôleur
UART Bluetooth Broadcom ; **l'image réelle ancienne à `hciattach` peut être
rejetée**, sans essai matériel à ce jour. Pas de choix automatique ni de
contournement. Seule notre annonce/connexion privée est retirée : aucun HCI
raw/mgmt, reset, overlay, changement NetworkManager/Wi-Fi ou interface vidéo.
Voir [backend BlueZ et limites](odid-bluez-backend-fr.md).

## Vérification hors cible

[Lanceur ARMhf](../tools/dev-rpi/test-odid.sh) : WSL Ubuntu-22.04, chroot
Bullseye ARMhf existant, GCC 10.2.1 ; staging ext4 séparé de la baseline et du
dépôt de travail (évite les inodes DrvFS non représentables par stat ARM32).
Il exécute uniquement le binaire de fixtures, jamais `openhd`. Vérifié hors cible :
build complet ARMhf `--full` avec les deux options ON et `--off` avec ODID OFF ;
fixtures du cœur et [mock BlueZ isolé sous UBSan](odid-bluez-backend-fr.md).
Le mode `--ubsan` couvre aussi les fixtures du cœur.
Le CMake autonome du dossier ODID expose également une cible de tests sans
dépendances vidéo/BlueZ/SDL de l'application.

Fixtures synthétiques : JSON partiel/invalide, persistance réelle, paramètres
invalides et callbacks périmés, filtrage source/cibles/id_or_mac, NaN/Inf,
masques 0..7 et invalides, timeout, reboot et wrap, plusieurs BasicID, SystemUpdate
partiel, timestamps, péremption, désactivation, changements concurrents,
arrêts répétés et callback actif à la fermeture de la barrière.

Validation FC réel, routage sur ArduPilot installé, réception/cadence RF,
coexistence vidéo/SSH, charge longue et conformité restent à réaliser.
Aucun déploiement ni test matériel/RF ODID effectué ; aucun service cible lancé.