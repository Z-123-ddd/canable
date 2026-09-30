// ============================================================================
//  EsiXml — 解析 EtherCAT 从站描述文件（ESI / ENI 的 XML）
//
//  从 XML 里抽取两类信息，供上位机把「操作的对象」对准上传的 XML：
//    · <Object>           → 对象字典（索引/子索引/名称/类型/位长/访问/默认值）
//    · <RxPdo>/<TxPdo>    → 默认 PDO 映射（SM、是否 Fixed、各 Entry 的索引与位长）
//  典型用法：导入厂商 ESI（如 ECAT_CIA402.xml）后：
//    - SDO 读写从对象字典里选对象；
//    - 使能流程用 RxPdo/TxPdo 推算控制字 0x6040 / 状态字 0x6041 在 PDO 内的偏移；
//    - 从站 PDO 为空时按 XML 写入分配（0x1C12/0x1C13）以促成进入 OP。
// ============================================================================
#ifndef ESIXML_H
#define ESIXML_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

// 对象字典一条（record 型对象的子项已展开，sub 即子索引）
struct EsiObjEntry {
    quint16    index = 0;
    quint8     sub = 0;
    QString    name;
    QString    type;        // UINT / DINT / ...
    int        bitSize = 0;
    QString    access;      // ro / rw / wo
    QByteArray def;         // <DefaultData> 原始字节（十六进制解出）
};

// 一条 PDO 映射项
struct EsiPdoEntry {
    quint16 index = 0;
    quint8  sub = 0;
    int     bitLen = 0;
    QString name;
};

// 一个 PDO（RxPdo 或 TxPdo）
struct EsiPdo {
    quint16 index = 0;      // PDO 映射对象索引（如 0x1600 / 0x1A00）
    int     sm = -1;        // 同步管理器号（Sm 属性）
    bool    fixed = false;  // Fixed="true"：映射项只读，只能改分配
    QString name;
    QVector<EsiPdoEntry> entries;
};

struct EsiXmlData {
    QString device;
    QVector<EsiObjEntry> objects;   // 全量对象字典（子项展开）
    QVector<EsiPdo>      rxPdos;    // 主站→从站
    QVector<EsiPdo>      txPdos;    // 从站→主站

    // 便捷查询
    const EsiObjEntry *findObject(quint16 index, quint8 sub) const;
    // 在 RxPdo/TxPdo 中查找某对象（含子索引）在 PDO 内的字节偏移；找不到返回 -1
    int controlwordByteOffset() const;   // 0x6040:0 在 RxPdo 内
    int statuswordByteOffset() const;    // 0x6041:0 在 TxPdo 内
};

// 从 XML 文本解析（便于无头自测）
bool parseEsiXml(const QByteArray &xml, EsiXmlData *out, QString *err);
// 从文件解析
bool loadEsiXml(const QString &path, EsiXmlData *out, QString *err);

#endif // ESIXML_H
