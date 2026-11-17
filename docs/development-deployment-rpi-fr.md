# Environnement de développement et déploiement Raspberry Pi

## État de préparation — 17 novembre 2026

Objectif : compiler le fork `2.7-evo` sans modification du code fonctionnel,
puis valider Air et Ground avant l'intégration OpenDroneID.

- Pi : Zero 2 W, Raspbian Bullseye armhf, OpenHD 2.6.4.
- Source de référence : `c75c37e6bcd014a990b81ae9b2556eb6afefd924` ; les commits
  suivants au moment de cet audit ne changent que la documentation.
- Windows : Git disponible ; Docker, CMake et Ninja non détectés dans le PATH.
- WSL2 opérationnel après redémarrage ; Ubuntu 22.04.5 installé.
- Environnement de build sur disque Linux sous `/opt/openhd-dev`, hors OneDrive.
- Chroot Raspberry Pi OS Lite Bullseye du 03/05/2023, ARMhf exécuté par QEMU ;
   GCC 10.2.1, CMake 3.18.4, Ninja, libcamera-openhd 1.2.10 installés.
- Build Release **réussi** du cœur à `1e84d4b8` ; `git diff c75c37e6 -- OpenHD`
   vide : aucune modification du code fonctionnel.
- Build Release **réussi** du compagnon OpenHD-SysUtils, révision
   `e89ce3d8541189c145c949cd04dc5b5f1d62cc83` du dépôt officiel.
- Deux paquets ARMhf produits et exportés dans `artifacts/rpi-baseline/`
   (répertoire ignoré par Git), avec manifestes et SHA-256 vérifiés après copie.
- Aucun déploiement 2.7 réalisé ; aucun service du Pi modifié.

**Étape actuelle : audit des dépendances et de la migration SysUtils sur le Pi
Air, puis sauvegarde et préparation d'un retour arrière autonome.** Un build
réussi n'est pas une validation matérielle.

### Artefacts de référence produits

| Paquet | Version | SHA-256 |
| --- | --- | --- |
| openhd | 2.7.0-1794903066-1e84d4b8 | `6fbcdd2eb417ab8a34f2c2b4f0aca72afc98bdaf7ee93ca48029d0ce83469940` |
| openhd-sys-utils | 1.0.3.20261117090242 | `7e3a5530d7901f3ad9fc553194462cd41d62e06ed2f76b20b017b84e419bc5c1` |

Les deux exécutables sont ELF 32 bits ARM EABI5, chargeur ARMhf ; aucune
bibliothèque manquante dans le chroot selon `ldd`. Ce contrôle reste à répéter
sur chaque Pi. SysUtils n'a pas été lancé : son démarrage peut configurer le
matériel, tenter un redimensionnement de partition et redémarrer le système.

Image officielle téléchargée depuis `downloads.raspberrypi.com`, empreinte
SHA-256 vérifiée :
`b5e3a1d984a7eaa402a6e078d707b506b962f6804d331dcc0daa61debae3a19a`.
Racine extraite depuis la partition commençant au secteur 532480 (512 octets),
image montée en lecture seule puis copiée avec préservation des permissions.

### Outillage local validé

- [Provisionnement](../tools/dev-rpi/provision-armhf.sh) : à lancer en root
   dans Ubuntu WSL, uniquement après extraction de l'image dans
   `/opt/openhd-dev/armhf-rootfs` et clonage des deux dépôts dans `source` et
   `sysutils-source`. Ne jamais lancer sur un Pi.
- [Build Release](../tools/dev-rpi/build-armhf.sh) : configure et compile dans
   `/build` du chroot, quatre tâches parallèles, journaux dans
   `/opt/openhd-dev/logs`. Rétablit les montages après extinction de WSL.
- [Packaging](../tools/dev-rpi/package-armhf.sh) : construit le paquet OpenHD
   avec `dpkg-deb`, mêmes hooks, unité et dépendances que le packaging upstream
   `standard armhf`, sans relancer son build en source. Produit aussi le paquet
   CPack officiel de SysUtils et les manifestes de versions/bibliothèques.
- [Garde anti-services](../tools/dev-rpi/policy-rc.d) : bloque les démarrages de
   services pendant les installations APT du chroot ; normalisé en LF à la copie.

Les sources fonctionnelles du fork et de SysUtils restent inchangées. Ce chroot
est une isolation de build/ABI, pas une barrière de sécurité : ne pas y exécuter
les démons de contrôle matériel ni des scripts non examinés.

## Chaîne de build de référence du dépôt

Le [workflow Raspberry Pi](../.github/workflows/build_package_rpi.yml) décrit
une compilation dans un Raspberry Pi OS émulé, base `raspios_lite:2023-05-03`,
avec 4 Go supplémentaires, sous runner Ubuntu.

Il appelle [install_build_dep.sh](../install_build_dep.sh) pour la plateforme
`rpi`, puis [package.sh](../package.sh) avec les arguments `standard armhf
raspbian bullseye`.

Le script d'installation ajoute le dépôt Cloudsmith OpenHD release et installe
notamment libcamera-openhd, les bibliothèques GStreamer, Poco, libpcap, libsodium,
libnl, SDL2 et les outils de compilation. Il peut purger des paquets caméra et
forcer des écrasements : **à exécuter uniquement dans l'environnement de build
jetable, pas directement sur les Pi opérationnels ni sur le Linux hôte**.

Le packaging configure CMake depuis la racine du dépôt, compile avec tous les
cœurs et utilise fpm. Il écrit dans `/out` ; créer ce répertoire dans le build
isolé. Le script supprime un autre répertoire temporaire que son répertoire de
staging réel : utiliser un environnement neuf et un staging vide pour chaque
build, sans modifier le script upstream à cette étape.

Les workflows historiques publient aussi sur Cloudsmith avec un secret de
mainteneur : cette publication n'est pas nécessaire pour nos paquets locaux et
ne doit pas être tentée. Ne pas considérer les workflows comme validés simplement
parce qu'ils sont présents dans le dépôt.

## Procédure de préparation

1. Vérifier WSL puis installer une distribution Ubuntu adaptée aux outils de build.
2. Construire dans le système de fichiers Linux, hors du dossier OneDrive monté,
   pour conserver permissions et liens symboliques et éviter les lenteurs.
3. Récupérer le fork et les sous-modules à une révision figée.
4. Préparer un environnement Raspberry Pi OS Bullseye **armhf** isolé, avec QEMU
   ou un mécanisme équivalent à celui du workflow officiel ; Ubuntu x86 seul
   ne produit pas un binaire utilisable sur les Pi.
5. Vérifier la disponibilité des paquets Cloudsmith et noter leurs versions.
6. Installer les dépendances dans cet environnement, configurer Release et
   compiler le cœur sans changer ses sources ; limiter le parallélisme selon
   la mémoire de l'environnement.
7. Produire le paquet et relever SHA-256, architecture, révision, dépendances et
   versions des bibliothèques. Conserver les journaux de build.

Ne pas télécharger ni exécuter aveuglément une chaîne de scripts distante :
examiner la configuration des dépôts et les opérations système prévues.

## Compatibilité à résoudre avant installation

La branche récente reçoit configuration et inventaire Wi-Fi depuis SysUtils.
Un build réussi n'assure pas que les services installés dans l'image 2.6.4
fournissent les API attendues.

Avant déploiement : inventorier les services SysUtils du Pi, comparer les
protocoles de socket de la branche et identifier les versions compagnon
nécessaires. Vérifier également QOpenHD, libcamera et les plugins GStreamer.
Ne pas installer le paquet 2.7 si ces préconditions ne sont pas satisfaites.

Le compagnon officiel est <https://github.com/OpenHD/OpenHD-SysUtils>.
Le dépôt APT release Bullseye propose SysUtils 1.0.0, tandis que le dépôt source
audité est en 1.0.3 : ne pas présumer que le candidat APT satisfait le protocole.
Le paquet compagnon local est donc préparé séparément, sans l'installer.
Le socket attendu par le cœur est `/run/openhd/openhd_sys.sock`.
Sans ce compagnon, l'inventaire Wi-Fi est vide et la plateforme peut rester
inconnue ; les anciens marqueurs Air/Ground seuls ne suffisent plus.

Avant le premier démarrage de SysUtils, examiner sa configuration et la logique
de premier boot : synchronisation des paramètres, configuration caméra,
montages et redimensionnement éventuel FAT32. Ne pas simplement démarrer ce
service sur l'image 2.6.4 sans préparation et sauvegarde des réglages de boot.

Préserver noyau et pilotes `rtl88x2eu_ohd` tant qu'aucune incompatibilité démontrée
n'impose une migration d'image. Aucune mise à jour générale de l'OS.

## Déploiement séquentiel et récupération

L'utilisateur autorise les essais 2.7 sur les deux Pi et dispose d'ImageWriter
pour restaurer une image stable. Cela ne remplace pas la sauvegarde des réglages
et ne garantit pas la récupération à distance.

1. Exporter sur le PC les paramètres, unités système pertinentes et la version
   actuelle ; conserver le binaire 2.6.4 et, si disponible, son paquet.
2. Transférer les paquets et dépendances par SSH/SFTP ; vérifier les empreintes.
3. Simuler la résolution des dépendances et vérifier l'espace libre ; pas de
   contournement forcé des dépendances ni d'installation partielle.
4. Examiner les hooks [before-install.sh](../before-install.sh) et
   [after-install.sh](../after-install.sh) avant leur exécution sur le Pi.
5. Préparer un retour arrière local autonome avant l'arrêt d'OpenHD : son arrêt
   peut supprimer le hotspot et couper SSH. Un simple `nohup` n'est pas une
   stratégie de rollback suffisante.
6. Déployer l'Air accessible, relever les preuves de fonctionnement, puis demander
   à l'utilisateur de connecter Windows au hotspot Ground pour répéter l'opération.
7. Valider la paire seulement quand les deux côtés utilisent la combinaison
   logicielle choisie ; ne pas conclure sur la vidéo avec une paire mixte.

Si les paquets nécessaires ne sont pas compatibles avec Bullseye, préparer une
image cohérente au lieu de multiplier les remplacements manuels de bibliothèques.

## Recette sans OpenDroneID

- Démarrage sans erreur et services compagnon disponibles.
- Hotspot/SSH retrouvés après démarrage et après redémarrage complet.
- Radios reconnues par matériel/PHY : Air intégré wlan0, USB wlan1 au relevé ;
  Ground intégré wlan1, USB wlan0 au relevé, sans garantie de stabilité des noms.
- Caméra IMX708 fonctionnelle sur Air, vidéo décodée sur Ground.
- UART FC et OSD conservés, modification des paramètres vérifiée.
- Appairage, fréquence et largeur de canal cohérents.
- Pas de régression de charge, mémoire, température, sous-tension ou latence.
- Aucun module OpenDroneID ajouté ou activé à ce jalon.

Résultats matériels détaillés et spécifications fonctionnelles :
[intégration OpenDroneID](opendroneid-integration-spec-fr.md).