#ifndef CIMGETIMAGE_H
#define CIMGETIMAGE_H
#include <QtEndian>
#include <QImage>
#include <QByteArray>
#include <QIODevice>
#include <QImageReader>
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMap>

#define EXIF_TIFF_LSB_MAGIC "Exif\x00\x00II\x2a\x00"
#define EXIF_TIFF_MSB_MAGIC "Exif\x00\x00MM\x00\x2a"
#define EXIF_TIFF_MAGIC_LEN 10

#define TIFF_HEADER_LEN 8
#define TIFF_IFD_ENTRY_LEN 12

#define EXIF_IDENTIFIER_LEN 6

#define EXIF_TYPE_SHORT 3

#define EXIF_TAG_ORIENTATION 0x112

/* Standalone markers without length information */
#define JPEG_MARKER_TEM  0x01
#define JPEG_MARKER_RST0 0xd0
#define JPEG_MARKER_RST1 0xd1
#define JPEG_MARKER_RST2 0xd2
#define JPEG_MARKER_RST3 0xd3
#define JPEG_MARKER_RST4 0xd4
#define JPEG_MARKER_RST5 0xd5
#define JPEG_MARKER_RST6 0xd6
#define JPEG_MARKER_RST7 0xd7
#define JPEG_MARKER_SOI  0xd8
#define JPEG_MARKER_EOI  0xd9

#define JPEG_MARKER_APP1 0xe1

#define JPEG_MARKER_PREFIX 0xff

static uchar getMarker(QFile &f)
{
    /* CCITT T.81 Annex B: "All markers are assigned two-byte
       codes: an XFF byte followed by a byte which is not equal
       to 0 or XFF (see Table B.1). Any marker may optionally be
       preceded by any number of fill bytes, which are bytes
       assigned code XFF." */

    uchar c;

    if (!f.getChar(reinterpret_cast<char*>(&c)) || c != JPEG_MARKER_PREFIX)
        return 0;

    while (c == JPEG_MARKER_PREFIX)
        if (f.getChar(reinterpret_cast<char*>(&c)) == false)
            return 0;

    if (c == 0) /* Not a marker */
        return 0;

    return c;
}

static quint16 getMarkerLength(QFile &f)
{
    char buf[2];

    if (f.read(buf, 2) != 2)
        return 0;
    return qFromBigEndian<quint16>(reinterpret_cast<uchar *>(buf));
}

static bool getExifData(QFile &f, QByteArray &data)
{
    uchar marker;
    quint16 len;
    qint64 skip;

    f.seek(0);

    marker = getMarker(f);
    if (marker != JPEG_MARKER_SOI)
        return false;
    while (true) {
        marker = getMarker(f);
        if (marker == 0)
            return false;

        switch (marker) {

        case JPEG_MARKER_SOI: /* shouldn't see this anymore */
        case JPEG_MARKER_EOI: /* end of the line, no EXIF in sight */
            return false;

        case JPEG_MARKER_TEM:
        case JPEG_MARKER_RST0:
        case JPEG_MARKER_RST1:
        case JPEG_MARKER_RST2:
        case JPEG_MARKER_RST3:
        case JPEG_MARKER_RST4:
        case JPEG_MARKER_RST5:
        case JPEG_MARKER_RST6:
        case JPEG_MARKER_RST7:
            /* Standalones, just skip */
            break;

        case JPEG_MARKER_APP1:
            /* CCITT T.81 Annex B:
                   The first parameter in a marker segment is the
                   two-byte length parameter. This length parameter
                   encodes the number of bytes in the marker segment,
                   including the length parameter and excluding the
                   two-byte marker. */
            len = getMarkerLength(f);
            if (len < 2)
                return false;
            data.resize(len - 2);
            if (f.read(data.data(), len - 2) != len - 2)
                return false;
            return true;

        default:
            /* Marker segment, just skip. */
            len = getMarkerLength(f);
            if (len < 2)
                return false;
            skip = f.pos() + static_cast<qint64>(len) - 2;
            f.seek(skip);
            if (f.pos() != skip)
                return false;
            break;
        }
    }
}

enum Orientation {
    TopLeft = 1,
    TopRight,
    BottomRight,
    BottomLeft,
    LeftTop,
    RightTop,
    RightBottom,
    LeftBottom
};

static QMap<QString, Orientation> OrientationCacheMap; // 缓存减少计算

static Orientation exifOrientationFromJpeg(const QString &fname)
{
    if(OrientationCacheMap.contains(fname)){
        return OrientationCacheMap.value(fname);
    }
    QByteArray data;
    const uchar *ptr;
    quint32 len;
    quint32 pos;
    bool msbFirst;
    quint32 ifdOff;
    quint16 fieldCount;
    quint16 o;

    QString fileName = fname;
    fileName = fileName.remove("file://");
    QFile f(fileName);
    if (f.open(QIODevice::ReadOnly) == false || getExifData(f, data) == false){
        qDebug() << "PhotosExtension=== exifOrientationFromJpeg Error 0" << fname << f.exists() << f.open(QIODevice::ReadOnly) << getExifData(f, data);
        goto exit;
    }

    ptr = reinterpret_cast<const uchar *>(data.constData());
    len = data.length();
    pos = 0;

    /* 6 bytes for Exif identifier, 8 bytes for TIFF header */
    if (len < EXIF_IDENTIFIER_LEN + TIFF_HEADER_LEN){
        qDebug() << "PhotosExtension=== exifOrientationFromJpeg Error 1";
        goto exit;
    }

    if (memcmp(ptr + pos, EXIF_TIFF_LSB_MAGIC, EXIF_TIFF_MAGIC_LEN) == 0)
        msbFirst = false;
    else if (memcmp(ptr + pos, EXIF_TIFF_MSB_MAGIC, EXIF_TIFF_MAGIC_LEN) == 0)
        msbFirst = true;
    else{
        qDebug() << "PhotosExtension=== exifOrientationFromJpeg Error 2";
        goto exit;
    }

    ifdOff = msbFirst
            ? qFromBigEndian<quint32>(ptr + pos + EXIF_TIFF_MAGIC_LEN)
            : qFromLittleEndian<quint32>(ptr + pos + EXIF_TIFF_MAGIC_LEN);

    /* IFD offset is measured from TIFF header and can't go backwards */
    if (ifdOff < TIFF_HEADER_LEN){
        qDebug() << "PhotosExtension=== exifOrientationFromJpeg Error 3";
        goto exit;
    }

    pos = EXIF_IDENTIFIER_LEN + ifdOff;

    if (len < pos + 2){
        qDebug() << "PhotosExtension=== exifOrientationFromJpeg Error 4";
        goto exit;
    }
    fieldCount = msbFirst
            ? qFromBigEndian<quint16>(ptr + pos)
            : qFromLittleEndian<quint16>(ptr + pos);
    pos += 2;
    if (len < pos + TIFF_IFD_ENTRY_LEN*static_cast<quint32>(fieldCount)){
        qDebug() << "PhotosExtension=== exifOrientationFromJpeg Error 5";
        goto exit;
    }

    for (quint16 f = 0; f < fieldCount; f++) {
        quint16 tag = msbFirst
                ? qFromBigEndian<quint16>(ptr + pos)
                : qFromLittleEndian<quint16>(ptr + pos);
        quint16 type = msbFirst
                ? qFromBigEndian<quint16>(ptr + pos + 2)
                : qFromLittleEndian<quint16>(ptr + pos + 2);
        quint32 num = msbFirst
                ? qFromBigEndian<quint32>(ptr + pos + 4)
                : qFromLittleEndian<quint32>(ptr + pos + 4);
        if (tag == EXIF_TAG_ORIENTATION &&
                type == EXIF_TYPE_SHORT &&
                num == 1) {
            o = msbFirst
                    ? qFromBigEndian<quint16>(ptr + pos + 8)
                    : qFromLittleEndian<quint16>(ptr + pos + 8);
            goto exit;
        } else {
            pos += TIFF_IFD_ENTRY_LEN;
        }
    }

    /* We're only interested in the 0th IFD, so quit when at its end */

exit:
    f.close();

    if (o < static_cast<quint16>(TopLeft) || o > static_cast<quint16>(LeftBottom)){
        OrientationCacheMap.insert(fname, TopLeft);
        return TopLeft;
    }

    OrientationCacheMap.insert(fname, static_cast<Orientation>(o));
    return static_cast<Orientation>(o);
}

static QMap<QString, QString> ImageFormatCacheMap; // 缓存减少计算

static QString GetImageFormatByFilePath(const QString &filePath)
{
    if(ImageFormatCacheMap.contains(filePath)){
        return ImageFormatCacheMap.value(filePath);
    }
    if(!filePath.isEmpty()) {
        static QMimeDatabase mimeDb;
        QString format = mimeDb.mimeTypeForFile(filePath, QMimeDatabase::MatchContent).preferredSuffix();
        if(format.isEmpty()){
            format = mimeDb.mimeTypeForFile(filePath, QMimeDatabase::MatchExtension).preferredSuffix();
        }
        ImageFormatCacheMap.insert(filePath, format);
        return format;
    }else{
        qDebug() << "PhotosExtension===GetImageDimension Error: filePath is empty";
    }
    return "NA";
}

static bool ReadImage(QIODevice *dev, QImage *image, const QSize &requestSize = QSize())
{
    QImageReader imgio(dev);
    QSize s = imgio.size();
    QSize readSize(1280,1280);
    if(requestSize.isValid()){
        readSize = requestSize;
    }else{
        qDebug() << "PhotosExtension===ReadImage Use 1280*1280";
    }
    if (imgio.format() == "svg" || imgio.format() == "svgz" || readSize.width() < s.width() || readSize.height() < s.height() ) {
        s.scale(readSize.width(), readSize.height(), Qt::KeepAspectRatio);
    }
    imgio.setScaledSize(s);
    return imgio.read(image);
}

static QImage GetImage(const QString &url, const QSize &requestSize = QSize())
{
    QString filePath = QUrl(url).toLocalFile();
    QImage srcImage;
    QFile f(filePath);

    if (f.open(QIODevice::ReadOnly) && ReadImage(&f, &srcImage, requestSize)) {
        f.close();

        if("jpeg" == GetImageFormatByFilePath(filePath)){
            switch (exifOrientationFromJpeg(filePath)) {
            case TopRight: /* horizontal flip */
                return srcImage.mirrored(true, false);
            case BottomRight: /* horizontal flip, vertical flip */
                return srcImage.mirrored(true, true);
            case BottomLeft: /* vertical flip */
                return srcImage.mirrored(false, true);
            case LeftTop: /* rotate 90 deg clockwise and flip horizontally */
                return srcImage.transformed(QTransform().rotate(90.0),Qt::SmoothTransformation).mirrored(true, false);
            case RightTop: /* rotate 90 deg anticlockwise */
                return srcImage.transformed(QTransform().rotate(90.0),Qt::SmoothTransformation);
            case RightBottom: /* rotate 90 deg anticlockwise and flip horizontally */
                return srcImage.transformed(QTransform().rotate(-90.0),Qt::SmoothTransformation).mirrored(true, false);
            case LeftBottom: /* rotate 90 deg clockwise */
                return srcImage.transformed(QTransform().rotate(-90.0),Qt::SmoothTransformation);
            default:
                return srcImage;
            }
        }
    }

    return srcImage;
}

static QImage rotate(const QImage &src,
                     Orientation orientation)
{
    QTransform trans;
    QImage dst, tmp;

    /* For square images 90-degree rotations of the pixel could be
       done in-place, and flips could be done in-place for any image
       instead of using the QImage routines which make copies of the
       data. */

    switch (orientation) {
        case TopRight:
            /* horizontal flip */
            dst = src.mirrored(true, false);
            break;
        case BottomRight:
            /* horizontal flip, vertical flip */
            dst = src.mirrored(true, true);
            break;
        case BottomLeft:
            /* vertical flip */
            dst = src.mirrored(false, true);
            break;
        case LeftTop:
            /* rotate 90 deg clockwise and flip horizontally */
            trans.rotate(90.0);
            tmp = src.transformed(trans);
            dst = tmp.mirrored(true, false);
            break;
        case RightTop:
            /* rotate 90 deg anticlockwise */
            trans.rotate(90.0);
            dst = src.transformed(trans);
            break;
        case RightBottom:
            /* rotate 90 deg anticlockwise and flip horizontally */
            trans.rotate(-90.0);
            tmp = src.transformed(trans);
            dst = tmp.mirrored(true, false);
            break;
        case LeftBottom:
            /* rotate 90 deg clockwise */
            trans.rotate(-90.0);
            dst = src.transformed(trans);
            break;
        default:
            dst = src;
            break;
    }

    return dst;
}

static QMap<QString, QSize> ImageDimensionCacheMap; // 缓存减少计算
#endif // CIMGETIMAGE_H
