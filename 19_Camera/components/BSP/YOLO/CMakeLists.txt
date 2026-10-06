idf_component_register(
    SRCS "yolo_person.cpp"
    INCLUDE_DIRS "."
    REQUIRES coco_detect log
)
