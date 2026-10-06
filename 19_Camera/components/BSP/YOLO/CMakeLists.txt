idf_component_register(
    SRCS "yolo_person.cpp"
    INCLUDE_DIRS "."
    REQUIRES pedestrian_detect log
)
