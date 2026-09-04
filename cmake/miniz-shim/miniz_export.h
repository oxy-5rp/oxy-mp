// Заглушка вместо заголовка, который порождает CMake miniz. Мы его CMake не
// подключаем (тянет свои цели), а собираем исходники сами — значит и этот
// заголовок заводим сами. Для статической сборки метки экспорта пусты.
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#endif
