"""SysY object-type contract for flat fixture checks."""


def global_type_failures(flat_module):
    failures = []
    for name, (object_type, _init) in flat_module.globals.items():
        # SysY globals are scalar objects or arrays of scalar objects.
        # A pointer inside the object type is an unstripped structured wrapper.
        if 'ptr[' in object_type:
            failures.append('全局对象 %s 的数组元素含指针：%s' %
                            (name, object_type))
    return failures
