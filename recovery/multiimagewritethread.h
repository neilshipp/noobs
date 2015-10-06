#ifndef MULTIIMAGEWRITETHREAD_H
#define MULTIIMAGEWRITETHREAD_H

#include <QThread>
#include <QMessageBox>
#include <QStringList>
#include <QMultiMap>
#include <QVariantList>

class MultiImageWriteThread : public QThread
{
    Q_OBJECT
public:
    explicit MultiImageWriteThread(QObject *parent = 0);
    void addImage(const QString &folder, const QString &flavour);

protected:
    virtual void run();
    void clearEBR();
    bool processImage(const QString &folder, const QString &flavour);
    bool reduceExtendedPartition(int sizeInSectors);
    QByteArray generateBCDSignature( quint32 diskSignature, quint64 partitionOffset);
    bool updateWindowsBCD( quint32 oldDiskSignature, quint32 newDiskSignature,
        quint64 oldEFIOffset, quint64 oldMainOsOffset,
        quint64 newEFIOffset, quint64 newMainOsOffset);
    quint32 getDiskSignature();
    bool addPartitionEntry(int sizeInSectors, int type, int specialOffset = 0);
    bool sfdisk(int part, int start, int size, const QByteArray &type);
    bool mkfs(const QByteArray &device, const QByteArray &fstype = "ext4", const QByteArray &label = "", const QByteArray &mkfsopt = "");
    bool dd(const QString &imagePath, const QString &device);
    bool untar(const QString &tarball);
    bool isLabelAvailable(const QByteArray &label);
    QByteArray getLabel(const QString part);
    QByteArray getUUID(const QString part);
    void patchConfigTxt();
    QString getDescription(const QString &folder, const QString &flavour);
    bool saveBootFiles();
    bool restoreBootFiles();

    /* key: folder, value: flavour */
    QMultiMap<QString,QString> _images;
    int _extraSpacePerPartition, _sectorOffset, _part;
    QVariantList installed_os;
    
signals:
    void error(const QString &msg);
    void statusUpdate(const QString &msg);
    void parsedImagesize(qint64 size);
    void completed();
    void runningMKFS();
    void finishedMKFS();

void query(const QString &msg, const QString &title, QMessageBox::StandardButton* answer);
    
public slots:
    
};

#endif // MULTIIMAGEWRITETHREAD_H
