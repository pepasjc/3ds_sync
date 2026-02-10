// Stub implementations for libnds functions when not using calico

void __libnds_exit(int rc) {
    while(1);
}

void __libnds_mpu_setup(void) {
    // MPU setup stub - not needed for basic DS functionality
}

int __dsimode = 0;

void __secure_area__(void) {
    // Secure area stub
}

void initSystem(void) {
    // Basic system init stub
}
