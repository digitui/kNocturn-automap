def parameter_banks(device):
    if device.class_name == 'OriginalSimpler':      # pretend Live has a curated bank
        ps = device.parameters[1:]
        return [(ps[5], ps[3], None, ps[1], ps[0], ps[2], ps[4], ps[6])]
    ps = device.parameters[1:]
    return [tuple(ps[i:i+8]) + (None,) * (8 - len(ps[i:i+8])) for i in range(0, len(ps), 8)]
