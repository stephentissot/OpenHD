# Backend ODID BLE Legacy via BlueZ — intégration opt-in expérimentale

## Statut et activation

Ce backend **expérimental** est câblé au worker de
[Manager](../OpenHD/ohd_telemetry/src/opendroneid/Core.cpp).
Les options CMake `OPENHD_ENABLE_ODID` et `OPENHD_ODID_BLUEZ` sont toutes deux
**OFF** par défaut ; les deux doivent être ON (voir
[CMake ODID](../OpenHD/ohd_telemetry/src/opendroneid/CMakeLists.txt)).
Au runtime, `ODID_ENABLE=0` et `ODID_DRY_RUN=1` restent les défauts sûrs.
Pour tenter une inscription : autorisation globale, DRY_RUN désactivé, demande
BLE, données complètes fraîches et `ODID_BT_DEV=hciN` explicite sont nécessaires.
Le callback UART ne fait aucune I/O radio. NAN et Beacon ne sont pas implémentés.

- [Interface](../OpenHD/ohd_telemetry/src/opendroneid/BluezBackend.h) :
  `openhd::odid::BluezBackend`, constructeur sans I/O, copie/déplacement interdits.
- [Implémentation](../OpenHD/ohd_telemetry/src/opendroneid/BluezBackend.cpp).
- `start_or_update(frame, counter, hci_name)` : chaîne vide seulement lorsque
  l'enregistrement courant a été accepté ; sinon raison ≤191 octets.
  Les erreurs distantes sont identifiées par leur nom D-Bus, sans recopier
  des messages arbitraires ou des données privées.
- `active()` : observation locale de l'enregistrement, pas mesure RF.
- `stop() noexcept` et destructeur : arrêt de notre seule annonce/connexion.
  Appeler `stop()` pour désactivation, données périmées ou autorisation retirée.

**Les appels publics, y compris `active()`, doivent être sérialisés par le
worker propriétaire**, jamais depuis le callback UART. La fraîcheur des données,
l'autorisation locale/FC, le DRY_RUN, la rotation des types et leurs compteurs
restent à la charge de `Manager`. `ODID_CAP=1` n'est publié qu'après une
inscription BlueZ locale courante réussie ; `ODID_EFFECTIVE=1` désigne également
cette inscription, pas une réception RF. ARM_STATUS reste toujours
`PRE_ARM_FAIL_GENERIC`, même après succès BlueZ, tant que la RF n'est pas validée.
Aucun résultat de test ne prouve la conformité.

## Données et comportement BlueZ

Objet exporté `org.bluez.LEAdvertisement1`, chemin dédié
`/org/openhd/odid/advertisement` sur une **connexion D-Bus privée**. Le chemin
est identique entre rotations, mais l'identité complète inclut le nom unique
de cette connexion : deux instances ne partagent pas l'annonce.

Seules propriétés exportées :

- `Type = "broadcast"` : annonce non connectable ; pas de GATT.
- `ServiceData` de signature `a{sv}`, entrée unique
  `0000fffa-0000-1000-8000-00805f9b34fb` contenant un `ay` de 27 octets :
  `0x0d`, compteur uint8 fourni, puis les 25 octets de trame dans leur ordre.

BlueZ construit l'en-tête Service Data UUID16. Le résultat attendu est
`1e 16 fa ff 0d <counter> <25 octets>`, soit **31 octets** d'Advertising Data.
Il ne faut pas préfixer le tableau D-Bus par `1e 16 fa ff`. Pas de LocalName,
ServiceUUIDs, Includes, Discoverable ou puissance supplémentaire : aucune
place disponible. Pas de SecondaryChannel/Coded PHY ; prototype destiné au
contrôleur BLE 4.2 intégré du Zero 2 W. Le PDU réellement choisi par la pile
reste à vérifier par capture indépendante ; aucune garantie de Legacy sur
un matériel/pile arbitraire n'est revendiquée.

BlueZ ignore les changements de propriétés après l'enregistrement. Si les
27 octets changent : `UnregisterAdvertisement` puis `RegisterAdvertisement`,
sur le même manager sélectionné et le même chemin. Si les octets sont identiques
et l'enregistrement encore actif, aucun nouvel enregistrement. **Cadence de
rotation, intervalle effectif, trous d'émission, identité/adresse RF et
coexistence Wi-Fi non prouvés.** `Manager` avance d'une trame à chaque
inscription réussie, puis attend 200 ms : la rotation comprend donc **200 ms
plus** le temps backend (budget d'attente jusqu'à 900 ms par inscription/mise
à jour), les accès sysfs et les désinscriptions. Ce n'est pas une période RF
de 200 ms ni un débit réglementaire garanti. Le transmetteur officiel Linux signale aussi
des limitations historiques des couches BlueZ haut niveau : le format vérifié
ne constitue pas une validation de cette méthode d'émission.

## Sélection restrictive et protection du système

Nom explicite canonique `hciN` (1–5 chiffres, pas de zéro initial sauf `hci0`).
Aucun choix automatique. Avant toute connexion D-Bus, lecture seule et bornée
en taille/profondeur de sysfs :

1. Compatible carte Device Tree commençant par `raspberrypi,`.
2. `device` de ce HCI résolu sous `/sys/devices`.
3. Ancêtre de sous-système `serial` ou `tty`, et compatible Bluetooth
   `brcm,bcm…-bt` (pas simplement le compatible SoC Broadcom).
4. Rejet de tout composant de chemin contenant `usb` ; preuves manquantes ou
   accès impossible : refus sûr, pas de contournement automatique.

Les anciens layouts `hciattach`, certains compatibles Broadcom sans suffixe
`-bt` et certains noyaux pourtant légitimes peuvent être rejetés volontairement.
**L'image réelle ancienne à `hciattach` peut donc être refusée ; aucun essai
matériel n'a encore vérifié cette sélection.**
Ces contrôles sont une politique conservatrice, pas une attestation inviolable
ni une réservation contre le hotplug/remplacement concurrent du contrôleur.

Connexion au socket système Linux `/run/dbus/system_bus_socket` ; pas de
connexion partagée obtenue via `g_bus_get()`, pas d'override d'adresse par
l'environnement en production. Le propriétaire unique courant d'`org.bluez`
est résolu puis utilisé directement pour les appels, vérifié après inscription
et surveillé via `NameOwnerChanged`. `Release` n'est accepté que de ce
propriétaire ; il efface l'état actif, même pendant l'inscription. Une
déconnexion du bus rend également `active()` faux. Pas d'auto-démarrage BlueZ.

Refus `NotPermitted`/occupé, `InvalidLength`, BlueZ absent, timeout, etc. :
raison explicite, état inactif, fermeture de notre connexion. Pas de tentative
de voler une instance ni de réinitialiser l'adaptateur. Les erreurs n'acquièrent
jamais automatiquement une autre radio. Une nouvelle tentative doit être
appelée explicitement ; `Manager` réessaie après un backoff de 5 s.

**Jamais** de socket HCI raw/mgmt, power/reset HCI, découverte de tous les objets,
désinscription globale, scan, shell réseau, changement NetworkManager/Wi-Fi,
overlay, chargement de firmware ou activation du contrôleur. BlueZ doit déjà
être opérationnel et autoriser l'enregistrement. Les annonces d'autres clients
ne sont pas modifiées ; BlueZ peut néanmoins multiplexer le temps radio et
le matériel intégré partage des ressources RF avec le hotspot.

## Dispatch, limites temporelles et récupération

La première demande valide crée une boucle `GMainLoop` avec son propre
`GMainContext` thread-default et un thread dédié **détaché**. L'export est effectué sur
ce thread. Le worker appelant peut donc attendre un appel D-Bus synchrone
pendant que BlueZ appelle `Properties.GetAll` et reçoit les propriétés sur le
thread de boucle. Le contexte global d'OpenHD n'est pas requis et aucun
verrou du gestionnaire ODID n'est pris.

- Connexion/authentification asynchrone annulable : attente appelant ≤500 ms.
- Chaque appel D-Bus : timeout explicite ≤500 ms, sans auto-start.
- Budget global des attentes d'un `start_or_update` : 900 ms ; les appels
  suivants consomment seulement le budget restant. Pas de second budget
  d'unregister ajouté lors du traitement d'une erreur.
- `stop()` : tentative d'unregister de notre annonce enregistrée uniquement,
  ≤500 ms, puis annulation/fermeture asynchrone de notre connexion privée.
  Une annonce déjà `Release` ne nécessite pas d'unregister.
- Une inscription dont le résultat est incertain après timeout est éliminée
  par la déconnexion de **notre** client, pas en devinant son état à distance.

Une donnée périmée entraîne une demande de retrait à l'itération suivante du
worker : attente de 200 ms plus opérations backend à attente bornée en cours,
puis désinscription. `Manager` revérifie aussi les données courantes, la
génération et l'arrêt après I/O, avant publication/retrait. Cela ne garantit
pas une date limite stricte de cessation RF.

Ce sont des bornes d'attente API, **pas du temps réel dur** : ordonnancement
Linux, accès sysfs local et allocations ne disposent pas de WCET garanti.
Les exceptions C++ récupérables sont contenues dans l'API ; une panne mémoire
fatale GLib/processus n'est pas récupérable. La boucle conserve son état jusqu'au
nettoyage et à la fin des opérations asynchrones, dont la fermeture de connexion.
Le thread privé n'est pas joint par `stop()` : **ni join complet ni absence
absolue de fuite ne sont prouvés**. La fermeture distante
et la cessation physique RF sont asynchrones et non démontrées en cas de
daemon/noyau bloqué ; `stop()` n'est pas une confirmation RF de silence.

## Dépendances et vérifications hors matériel

Production : C++17, threads POSIX (`-pthread`), Gio/GLib/GObject via
`pkg-config gio-2.0` (`libglib2.0-dev`), filesystem standard GCC 10 ; pas de
`libbluetooth`. Gio n'est requis par le module que si `OPENHD_ODID_BLUEZ=ON`.
Sur la cible : BlueZ avec `LEAdvertisingManager1` et permissions D-Bus adaptées,
sans proposition d'élargir ici les droits système. Aucune dépendance n'est
installée automatiquement.

- [Tests isolés](../OpenHD/ohd_telemetry/src/opendroneid/BluezBackend_test.cpp) :
  translation unit autonome, inclut l'implémentation avec un define de test.
  Ne jamais compiler cette translation unit dans OpenHD ni la lier de nouveau
  avec l'implémentation. Ses overrides de bus/sysfs n'existent qu'à la compilation
  de test, jamais comme variables d'environnement de production.
- [Lanceur ARMhf](../OpenHD/ohd_telemetry/src/opendroneid/BluezBackend_test.sh) :
  utilise le chroot de développement WSL déjà installé, staging temporaire ext4
  nettoyé en sortie. Ne monte
  pas sysfs et ne lance aucun service système. Dépendances supplémentaires de
  test : `gio-unix-2.0`, `dbus-daemon`, GCC/UBSan et pkg-config déjà disponibles.
- Vérifié le 17 novembre 2026 : **ARMhf GCC 10 / Gio 2.66.8**, production
  `-fsyntax-only -Wall -Wextra -Werror`, exécutable de test sous UBSan
  `halt_on_error=1` et GLib `fatal-warnings` : succès.
- Bus D-Bus privé et sysfs synthétique : refus nom/HCI absent/non-Pi/sans preuve
  UART/sans compatible Bluetooth/USB ; construction/arrêt sans connexion ;
  connexion échouée et authentification bloquée ; BlueZ absent ; GetAll requis
  avant réponse Register ; propriétés et chaque octet du payload ; rotation
  et compteurs 254/255/0 ; répétition identique ; arrêt idempotent ; Release
  autorisé/refusé ; Release pendant Register ; refus occupé/longueur ; timeout
  Register/Unregister ; disparition du client après erreur ; propriétaires
  indépendants ; perte de nom BlueZ.

Le [lanceur d'intégration](../tools/dev-rpi/test-odid.sh) a aussi validé le
build complet ARMhf avec `--full` (les deux options ON) et `--off` (ODID OFF).
Les mocks isolés sous UBSan ne valident ni la RF ni le comportement du matériel.
Aucun déploiement ODID, lancement d'OpenHD, test matériel/RF, intervention
Air/Ground ou modification réseau effectué dans ces vérifications.

## Sources officielles consultées

1. [OpenDroneID transmitter-linux, bluetooth.c](https://raw.githubusercontent.com/opendroneid/transmitter-linux/master/bluetooth.c),
   `hci_le_set_advertising_data` : UUID `FA FF`, code application `0D`, compteur
   puis `ODID_MESSAGE_SIZE` octets ; commentaire BLE Legacy et limites des
   couches haut niveau. **Seul le format est repris**, jamais ses resets,
   sélection automatique HCI ou suppressions globales.
2. [BlueZ LEAdvertisement](https://raw.githubusercontent.com/bluez/bluez/master/doc/org.bluez.LEAdvertisement.rst) :
   `Type`, UUID128 compressibles, `ServiceData`, sémantique `Release`.
3. [BlueZ Advertising API](https://bluez.readthedocs.io/en/latest/advertising-api/) :
   propriétés lues à l'enregistrement, modifications ignorées ; re-register
   nécessaire, capacité limitée, refus et limite Legacy de 31 octets.
4. [BlueZ LEAdvertisingManager](https://raw.githubusercontent.com/bluez/bluez/master/doc/org.bluez.LEAdvertisingManager.rst) :
   manager explicite par HCI, chemin identique pour unregister,
   `NotPermitted`, `AlreadyExists`, `InvalidLength`.
5. [Gio register_object](https://docs.gtk.org/gio/method.DBusConnection.register_object.html) :
   callbacks exécutés dans le contexte thread-default du thread d'export ;
   `Properties`, introspection et Peer gérés automatiquement.
6. [Binding Linux v6.6 Broadcom Bluetooth](https://raw.githubusercontent.com/torvalds/linux/v6.6/Documentation/devicetree/bindings/net/broadcom-bluetooth.yaml) :
   contrôleurs Broadcom attachés UART et compatible `brcm,bcm43438-bt`.

Références récupérées via HTTP, sans copier d'implémentation du transmetteur
officiel. Les liens `master` sont évolutifs ; ils ne constituent pas une
dépendance épinglée ni un résultat de qualification du BlueZ installé.